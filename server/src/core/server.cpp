#include "project/server.hpp"
#include "project/utils.hpp"
#include "project/config.hpp"
#include "database/postgres_connection.hpp"
#include "database/database_writer.hpp"
#include "database/json_generator.hpp"
#include <databento/dbn_file_store.hpp>
#include <databento/record.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <thread>
#include <random>
#include <algorithm>
#include <cmath>
// Use JSON from databento dependencies
#include <nlohmann/json.hpp>

// Include uWebSockets
#include "App.h"

namespace db = databento;

WebSocketServer::WebSocketServer(int port, const PostgresConnection::Config& dbConfig,
                                  size_t topLevels,
                                  bool outputFullBook,
                                  size_t ringBufferSize)
    : port_(port)
    , databaseConfig_(dbConfig)
    , isServerRunning_(false)
    , totalMessagesProcessed_(0)
    , totalBytesReceived_(0)
    , orderBook_(std::make_unique<Book>())
    , snapshotRingBuffer_(std::make_unique<RingBuffer<MboMessageWrapper>>(ringBufferSize))
    , symbol_("")  // Will be extracted from DBN file
    , topLevels_(topLevels)
    , outputFullBook_(outputFullBook)
    , processingMessagesReceived_(0)
    , processingOrdersProcessed_(0)
    , processingTotalTimeNs_(0)
    , processingTimingSamples_(0)
    , processingTimingReservoir_()
    , processingRng_(std::random_device{}())
{
    // Initialize order book (symbol will be set from DBN file metadata)
    orderBook_->setTopLevels(topLevels_);
    orderBook_->setOutputFullBook(outputFullBook_);
    
    // Initialize timing reservoir
    processingTimingReservoir_.reserve(kTimingReservoirSize);
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start() {
    if (isServerRunning_) {
        return false;
    }
    
    // Initialize database writer and JSON generator
    try {
        databaseWriter_ = std::make_unique<project::DatabaseWriter>(databaseConfig_);
        jsonGenerator_ = std::make_unique<project::JSONGenerator>(databaseConfig_);
    } catch (const std::exception& e) {
        utils::logError("Failed to initialize database writer: " + std::string(e.what()));
        return false;
    }
    
    // Set isServerRunning_ to true BEFORE starting the thread to avoid race condition
    isServerRunning_ = true;
    
    // Start database writer thread (jthread with stop token)
    databaseWriterThread_ = std::jthread([this](std::stop_token st) {
        this->databaseWriterLoop(st);
    });
    
    // Give the thread a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Start uWebSockets server
    uWS::App().ws<WebSocketServer::PerSocketData>("/*", {
        .compression = uWS::CompressOptions(uWS::DEDICATED_COMPRESSOR | uWS::DEDICATED_DECOMPRESSOR),
        .maxPayloadLength = 100 * 1024 * 1024, // 100MB
        .idleTimeout = 16,
        .maxBackpressure = 100 * 1024 * 1024,
        .closeOnBackpressureLimit = false,
        .resetIdleTimeoutOnSend = false,
        .sendPingsAutomatically = true,
        .upgrade = nullptr,
        .open = [this](auto *ws) {
            auto *data = ws->getUserData();
            data->buffer.clear();
            data->totalBytesReceived = 0;
            data->isMetadataReceived = false;
            data->fileName.clear();
            data->fileSize = 0;
            data->isProcessingStarted = false;
            
            // Store callback to send messages from processing thread
            data->sendMessage = [ws](const std::string& message) {
                ws->send(message, uWS::OpCode::TEXT, false);
            };
            
            // Send connection confirmation
            nlohmann::json response;
            response["type"] = "connected";
            response["message"] = "WebSocket connected. Send file metadata first.";
            ws->send(response.dump(), uWS::OpCode::TEXT, false);
        },
        .message = [this](auto *ws, std::string_view message, uWS::OpCode opCode) {
            auto *data = ws->getUserData();
            
            if (opCode == uWS::OpCode::TEXT) {
                // Handle JSON metadata
                try {
                    auto json = nlohmann::json::parse(message);
                    if (json.contains("type") && json["type"] == "metadata") {
                        data->isMetadataReceived = true;
                        data->fileName = json.value("fileName", "");
                        data->fileSize = json.value("fileSize", 0);
                        
                        // Create temporary file for DBN data
                        std::filesystem::path tempDir = std::filesystem::temp_directory_path();
                        data->tempFilePath = (tempDir / ("dbn_upload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dbn")).string();
                        
                        // Open temp file for writing
                        std::ofstream tempFile(data->tempFilePath, std::ios::binary);
                        if (!tempFile.is_open()) {
                            nlohmann::json error;
                            error["type"] = "error";
                            error["error"] = "Failed to create temporary file";
                            ws->send(error.dump(), uWS::OpCode::TEXT, false);
                            return;
                        }
                        tempFile.close();
                        
                        nlohmann::json response;
                        response["type"] = "metadata_received";
                        response["message"] = "Ready to receive file data";
                        ws->send(response.dump(), uWS::OpCode::TEXT, false);
                    } else if (json.contains("type") && json["type"] == "complete") {
                        // Process the complete DBN file (if not already started)
                        if (!data->isProcessingStarted && !data->tempFilePath.empty() && std::filesystem::exists(data->tempFilePath)) {
                            data->isProcessingStarted = true;
                            
                            // Send status update
                            nlohmann::json status;
                            status["type"] = "stats";
                            status["status"] = "Processing file...";
                            status["messagesProcessed"] = 0;
                            ws->send(status.dump(), uWS::OpCode::TEXT, false);
                            
                            // Process in a separate thread to avoid blocking
                            std::string tempPath = data->tempFilePath;
                            std::function<void(const std::string&)> sendMsg = data->sendMessage;
                            
                            std::jthread([this, tempPath, sendMsg](std::stop_token) {
                                PerSocketData tempData;
                                tempData.tempFilePath = tempPath;
                                processDbnChunk({}, &tempData, sendMsg);
                            }).detach();
                        }
                    }
                } catch (const std::exception& e) {
                    nlohmann::json error;
                    error["type"] = "error";
                    error["error"] = "Failed to parse metadata: " + std::string(e.what());
                    ws->send(error.dump(), uWS::OpCode::TEXT, false);
                }
            } else if (opCode == uWS::OpCode::BINARY) {
                // Handle binary DBN data
                if (!data->isMetadataReceived) {
                    nlohmann::json error;
                    error["type"] = "error";
                    error["error"] = "Metadata must be sent first";
                    ws->send(error.dump(), uWS::OpCode::TEXT, false);
                    return;
                }
                
                // Append to buffer and write to temp file
                std::vector<uint8_t> chunk(message.begin(), message.end());
                data->buffer.insert(data->buffer.end(), chunk.begin(), chunk.end());
                totalBytesReceived_ += chunk.size();
                
                // Write chunk to temp file
                std::ofstream tempFile(data->tempFilePath, std::ios::binary | std::ios::app);
                if (tempFile.is_open()) {
                    tempFile.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                    tempFile.close();
                }
                
                // Send progress update
                nlohmann::json progress;
                progress["type"] = "progress";
                progress["bytesReceived"] = data->buffer.size();
                progress["fileSize"] = data->fileSize;
                ws->send(progress.dump(), uWS::OpCode::TEXT, false);
                
                // Check if file upload is complete and automatically start processing
                if (!data->isProcessingStarted && data->buffer.size() >= data->fileSize && data->fileSize > 0) {
                    data->isProcessingStarted = true;
                    
                    // Send status update
                    nlohmann::json status;
                    status["type"] = "stats";
                    status["status"] = "Processing file...";
                    status["messagesProcessed"] = 0;
                    ws->send(status.dump(), uWS::OpCode::TEXT, false);
                    
                    // Process the complete DBN file in a separate thread
                    std::string tempPath = data->tempFilePath;
                    std::function<void(const std::string&)> sendMsg = data->sendMessage;
                    
                    std::jthread([this, tempPath, sendMsg](std::stop_token) {
                        PerSocketData tempData;
                        tempData.tempFilePath = tempPath;
                        processDbnChunk({}, &tempData, sendMsg);
                    }).detach();
                }
            }
        },
        .close = [this](auto *ws, int code, std::string_view message) {
            auto *data = ws->getUserData();
            
            // Process any remaining data if not already processing
            if (!data->isProcessingStarted && !data->tempFilePath.empty() && std::filesystem::exists(data->tempFilePath)) {
                    data->isProcessingStarted = true;
                    std::string tempPath = data->tempFilePath;
                    std::function<void(const std::string&)> sendMsg = data->sendMessage;
                    
                std::jthread([this, tempPath, sendMsg](std::stop_token) {
                        PerSocketData tempData;
                        tempData.tempFilePath = tempPath;
                        processDbnChunk({}, &tempData, sendMsg);
                    }).detach();
            }
            
            // Clean up temp file after a delay (to allow processing to complete)
            if (!data->tempFilePath.empty()) {
                std::thread([tempPath = data->tempFilePath]() {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    std::filesystem::remove(tempPath);
                }).detach();
            }
        }
    }).get("/status/:session_id", [this](auto *res, auto *req) {
        // Check if session is complete
        res->writeHeader("Content-Type", "application/json");
        res->writeHeader("Access-Control-Allow-Origin", "*");
        
        std::string sessionId = std::string(req->getParameter(0));
        
        if (sessionId.empty() || !jsonGenerator_) {
            res->writeStatus("400 Bad Request");
            res->end("{\"error\":\"Invalid session ID\"}");
            return;
        }
        
        // Query database for session status
        std::string sql = "SELECT status FROM processing_sessions WHERE session_id = '" + sessionId + "'";
        auto result = jsonGenerator_->getConnection().execute(sql);
        
        nlohmann::json response;
        if (result.success && !result.rows.empty()) {
            std::string status = result.rows[0][0];
            response["sessionId"] = sessionId;
            response["status"] = status;
            response["complete"] = (status == "completed");
        } else {
            response["error"] = "Session not found";
            response["complete"] = false;
        }
        
        res->writeStatus("200 OK");
        res->end(response.dump());
    }).get("/download/json", [this](auto *res, auto *req) {
        // Generate JSON from database on-demand
        res->writeStatus("200 OK");
        res->writeHeader("Content-Type", "application/json");
        res->writeHeader("Content-Disposition", "attachment; filename=\"order_book_output.json\"");
        
        // Get session_id from query parameter if provided, otherwise use latest
        std::string sessionId;
        std::string query = std::string(req->getQuery());
        
        if (query.find("session_id=") != std::string::npos) {
            size_t start = query.find("session_id=") + 11;
            size_t end = query.find("&", start);
            sessionId = (end == std::string::npos) ? query.substr(start) : query.substr(start, end - start);
        }
        
        // Generate JSON from database
        std::string jsonData;
        if (!sessionId.empty() && jsonGenerator_) {
            jsonData = jsonGenerator_->generateJSON(sessionId);
        } else if (jsonGenerator_ && !symbol_.empty()) {
            jsonData = jsonGenerator_->generateJSONForSymbol(symbol_);
        } else {
            jsonData = "{\"error\":\"No data available\"}";
        }
        
        res->end(jsonData);
    }).get("/*", [](auto *res, auto */*req*/) {
        // Serve static HTML file
        res->writeStatus("200 OK");
        res->writeHeader("Content-Type", "text/html");
        
        // Read and serve index.html
        std::ifstream file("../static/index.html");
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            res->end(buffer.str());
        } else {
            res->writeStatus("404 Not Found");
            res->end("File not found");
        }
    }).listen(port_, [this](auto *listen_socket) {
        if (!listen_socket) {
            std::cerr << "Failed to listen on port " << port_ << std::endl;
            isServerRunning_ = false;
        }
    }).run();
    
    return true;
}

void WebSocketServer::processDbnChunk(const std::vector<uint8_t>& /*chunk*/, PerSocketData* socketData,
                                      const std::function<void(const std::string&)>& sendMessage) {
    if (socketData->tempFilePath.empty() || !std::filesystem::exists(socketData->tempFilePath)) {
        return;
    }
    
    try {
        // Use DbnFileStore to read the temporary file
        db::DbnFileStore store(socketData->tempFilePath);
        
        // Extract symbol from DBN file metadata
        auto metadata = store.GetMetadata();
        if (!metadata.symbols.empty()) {
            symbol_ = metadata.symbols[0];  // Use first symbol from the file
            orderBook_->setSymbol(symbol_);
            std::cout << "Processing " << symbol_ << "..." << std::endl;
        } else {
            utils::logWarning("No symbols found in DBN file metadata");
        }
        
        // Start database session before processing
        if (databaseWriter_ && !symbol_.empty()) {
            // Extract file name from temp file path
            std::filesystem::path filePath(socketData->tempFilePath);
            std::string fileName = filePath.filename().string();
            size_t fileSize = std::filesystem::file_size(socketData->tempFilePath);
            
            try {
                databaseWriter_->startSession(symbol_, fileName, fileSize);
                activeSessionId_ = databaseWriter_->getCurrentSessionId();  // Cache it - never touch databaseWriter_ again from this thread
            } catch (const std::exception& e) {
                utils::logError("Failed to start database session: " + std::string(e.what()));
            }
        }
        
        // Start timing when processing begins (from pressing start button)
        processingStartTime_ = std::chrono::steady_clock::now();
        
        // Reset statistics for this processing session
        processingMessagesReceived_ = 0;
        processingOrdersProcessed_ = 0;
        processingTotalTimeNs_ = 0;
        processingTimingSamples_ = 0;
        processingTimingReservoir_.clear();
        
        // Process all records
        const db::Record* record;
        bool timingStarted = false;
        while ((record = store.NextRecord()) != nullptr) {
            if (record->RType() == db::RType::Mbo) {
                const auto& mbo = record->Get<db::MboMsg>();
                
                // Start timing on first processed message (same as receiver version)
                if (!timingStarted) {
                    processingStartTime_ = std::chrono::steady_clock::now();
                    timingStarted = true;
                }
                
                // Count message as received
                processingMessagesReceived_++;
                
                try {
                    auto applyStart = std::chrono::steady_clock::now();
                    
                    // Apply to order book
                    orderBook_->Apply(mbo);
                    
                    // Capture snapshot
                    BookSnapshot snap;
                    snap.symbol = symbol_;
                    snap.ts_ns = mbo.hd.ts_event.time_since_epoch().count();
                    
                    // Get BBO
                    auto bbo = orderBook_->Bbo();
                    snap.bid = bbo.first;
                    snap.ask = bbo.second;
                    snap.total_orders = orderBook_->GetOrderCount();
                    snap.bid_levels = orderBook_->GetBidLevelCount();
                    snap.ask_levels = orderBook_->GetAskLevelCount();
                    
                    // Get top levels
                    size_t bidCount = std::min(topLevels_, orderBook_->GetBidLevelCount());
                    size_t askCount = std::min(topLevels_, orderBook_->GetAskLevelCount());
                    snap.bids.reserve(bidCount);
                    snap.asks.reserve(askCount);
                    
                    for (size_t i = 0; i < bidCount; ++i) {
                        auto lvl = orderBook_->GetBidLevel(i);
                        if (!lvl || lvl.price == db::kUndefPrice) break;
                        snap.bids.push_back(LevelEntry{lvl.price, lvl.size, lvl.count});
                    }
                    
                    for (size_t i = 0; i < askCount; ++i) {
                        auto lvl = orderBook_->GetAskLevel(i);
                        if (!lvl || lvl.price == db::kUndefPrice) break;
                        snap.asks.push_back(LevelEntry{lvl.price, lvl.size, lvl.count});
                    }
                    
                    auto applyEnd = std::chrono::steady_clock::now();
                    const uint64_t elapsedNs = static_cast<uint64_t>(std::chrono::nanoseconds(applyEnd - applyStart).count());
                    processingTotalTimeNs_ += elapsedNs;
                    ++processingTimingSamples_;
                    
                    // Update timing reservoir (same as receiver version)
                    if (processingTimingReservoir_.size() < kTimingReservoirSize) {
                        processingTimingReservoir_.push_back(elapsedNs);
                    } else {
                        std::uniform_int_distribution<uint64_t> dist(0, processingTimingSamples_ - 1);
                        const uint64_t idx = dist(processingRng_);
                        if (idx < kTimingReservoirSize) {
                            processingTimingReservoir_[static_cast<size_t>(idx)] = elapsedNs;
                        }
                    }
                    processingOrdersProcessed_++;
                    
                    // Push snapshot to ring buffer for database writing
                    MboMessageWrapper wrapper(snap);
                    snapshotRingBuffer_->push(wrapper);
                    
                    totalMessagesProcessed_++;
                    
                    // Send periodic status updates (every 1000 messages)
                    if (sendMessage && totalMessagesProcessed_ % 1000 == 0) {
                        nlohmann::json stats;
                        stats["type"] = "stats";
                        stats["status"] = "Processing...";
                        stats["messagesProcessed"] = totalMessagesProcessed_.load();
                        sendMessage(stats.dump());
                    }
                } catch (const std::invalid_argument& e) {
                    // Handle missing orders/levels gracefully (same as src version)
                    std::string error_msg = e.what();
                    if (error_msg.find("No order with ID") != std::string::npos ||
                        error_msg.find("Received event for unknown level") != std::string::npos) {
                        // Skip silently (quiet)
                    } else {
                        throw;
                    }
                } catch (const std::exception& e) {
                    utils::logError("Error processing order: " + std::string(e.what()));
                }
            }
        }
        
        std::cout << "Processed " << totalMessagesProcessed_.load() << " messages" << std::endl;
        
        // End timing when processing completes
        processingEndTime_ = std::chrono::steady_clock::now();
        
        // Capture stats for DB thread to write (do this BEFORE setting isServerRunning_ = false)
        sessionStats_.messagesReceived = processingMessagesReceived_.load();
        sessionStats_.ordersProcessed = processingOrdersProcessed_.load();
        sessionStats_.throughput = getThroughput();
        sessionStats_.avgProcessNs = static_cast<int64_t>(getAverageOrderProcessNs());
        sessionStats_.p99ProcessNs = getP99OrderProcessNs();
            
        // Memory fence: ensure all sessionStats_ writes are visible before setting isServerRunning_ = false
        std::atomic_thread_fence(std::memory_order_release);
        
        // Signal that processing is complete - DB thread will exit when buffer is empty
        isServerRunning_.store(false, std::memory_order_release);
        
        // WAIT for DB writes to complete before sending completion message
        if (databaseWriterThread_.joinable()) {
            databaseWriterThread_.join();
        }
        
        // Send completion message - everything is done including DB writes
        // JSON generation happens on-demand when user clicks download
        if (sendMessage) {
            nlohmann::json complete;
            complete["type"] = "complete";
            complete["messagesReceived"] = processingMessagesReceived_.load();
            complete["ordersProcessed"] = processingOrdersProcessed_.load();
            complete["messagesProcessed"] = totalMessagesProcessed_.load();
            complete["bytesReceived"] = totalBytesReceived_.load();
            complete["dbWritesPending"] = snapshotRingBuffer_->size();
            complete["sessionId"] = activeSessionId_;  // Use cached value - don't touch databaseWriter_
            
            // Calculate throughput
            double throughput = getThroughput();
            complete["messageThroughput"] = throughput;
            
            // Calculate order processing statistics
            double avgNs = getAverageOrderProcessNs();
            uint64_t p99Ns = getP99OrderProcessNs();
            if (avgNs > 0.0) {
                double ordersPerSec = 1e9 / avgNs;
                complete["averageOrderProcessNs"] = avgNs;
                complete["p99OrderProcessNs"] = p99Ns;
                complete["orderProcessingRate"] = ordersPerSec;
            }
            
            // Final order book summary
            if (orderBook_) {
                complete["activeOrders"] = orderBook_->GetOrderCount();
                complete["bidPriceLevels"] = orderBook_->GetBidLevelCount();
                complete["askPriceLevels"] = orderBook_->GetAskLevelCount();
                
                auto finalBbo = orderBook_->Bbo();
                auto finalBid = finalBbo.first;
                auto finalAsk = finalBbo.second;
                
                if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
                    double bidValue = static_cast<double>(finalBid.price) / 1000000000.0;
                    double askValue = static_cast<double>(finalAsk.price) / 1000000000.0;
                    double spreadValue = std::abs(askValue - bidValue);
                    
                    complete["bestBid"] = bidValue;
                    complete["bestBidSize"] = finalBid.size;
                    complete["bestBidCount"] = finalBid.count;
                    complete["bestAsk"] = askValue;
                    complete["bestAskSize"] = finalAsk.size;
                    complete["bestAskCount"] = finalAsk.count;
                    complete["bidAskSpread"] = spreadValue;
                }
            }
            
            std::cout << "Session complete: " << activeSessionId_ << std::endl;
            sendMessage(complete.dump());
        }
        
        // Capture final book state for DB thread to write
        if (orderBook_) {
            auto finalBbo = orderBook_->Bbo();
            auto finalBid = finalBbo.first;
            auto finalAsk = finalBbo.second;
            
            if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
                sessionStats_.totalOrders = orderBook_->GetOrderCount();
                sessionStats_.bidLevels = orderBook_->GetBidLevelCount();
                sessionStats_.askLevels = orderBook_->GetAskLevelCount();
                sessionStats_.bestBid = static_cast<double>(finalBid.price) / 1000000000.0;
                sessionStats_.bestAsk = static_cast<double>(finalAsk.price) / 1000000000.0;
                sessionStats_.spread = std::abs(sessionStats_.bestAsk - sessionStats_.bestBid);
                sessionStats_.hasBookState = true;
                
                // Memory fence: ensure all writes are visible
                std::atomic_thread_fence(std::memory_order_release);
            }
        }
        
        // Note: DB thread will write stats and end session when it finishes
        
        // Clean up order book after processing completes
        if (orderBook_) {
            orderBook_->Clear();
        }
        
        
    } catch (const std::exception& e) {
        utils::logError("Error processing DBN file: " + std::string(e.what()));
        
        // End database session with error
        if (databaseWriter_) {
            databaseWriter_->endSession(false, e.what());
        }
        
        // Send error message to client
        if (sendMessage) {
            nlohmann::json error;
            error["type"] = "error";
            error["error"] = "Error processing DBN file: " + std::string(e.what());
            sendMessage(error.dump());
        }
    }
}

void WebSocketServer::databaseWriterLoop(std::stop_token stopToken) {
    // Drop indexes for faster bulk loading
    if (databaseWriter_) {
        databaseWriter_->dropIndexes();
    }
    
    size_t itemsWritten = 0;
    constexpr size_t kBatchSize = 5000;  // Larger batches for COPY command
    std::vector<MboMessageWrapper> batch;
    batch.reserve(kBatchSize);
    
    auto writeBatch = [&]() {
        if (batch.empty()) return;
        
        if (databaseWriter_ && databaseWriter_->writeBatch(batch)) {
                itemsWritten += batch.size();
        }
        batch.clear();
    };
    
    MboMessageWrapper wrapper;
    
    // Continue until processing done AND buffer empty
    while (isServerRunning_.load(std::memory_order_acquire) || !snapshotRingBuffer_->empty()) {
        if (snapshotRingBuffer_->try_pop(wrapper)) {
            batch.push_back(std::move(wrapper));
            
            // Flush when batch is full
            if (batch.size() >= kBatchSize) {
                writeBatch();
            }
        } else {
            // Buffer empty - flush pending batch
            writeBatch();
            
            // Exit if done
            if (!isServerRunning_.load(std::memory_order_acquire) && snapshotRingBuffer_->empty()) {
                break;
            }
            
            // Avoid busy-wait
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Final flush of any remaining items
    writeBatch();
    
    // Recreate indexes after bulk load complete
    if (databaseWriter_) {
        databaseWriter_->recreateIndexes();
    }
    
    // Write session stats and close session after all writes complete
    if (databaseWriter_) {
        // Memory fence: ensure we see all writes from processing thread
        std::atomic_thread_fence(std::memory_order_acquire);
        
        // Update session stats
        if (sessionStats_.messagesReceived > 0) {
            databaseWriter_->updateSessionStats(
                sessionStats_.messagesReceived,
                sessionStats_.ordersProcessed,
                sessionStats_.throughput,
                sessionStats_.avgProcessNs,
                sessionStats_.p99ProcessNs
            );
        }
        
        // Update final book state
        if (sessionStats_.hasBookState) {
            databaseWriter_->updateFinalBookState(
                sessionStats_.totalOrders,
                sessionStats_.bidLevels,
                sessionStats_.askLevels,
                sessionStats_.bestBid,
                sessionStats_.bestAsk,
                sessionStats_.spread
            );
        }
        
        // End session
        databaseWriter_->endSession(true);
    }
}

void WebSocketServer::stop() {
    if (!isServerRunning_) {
        return;
    }
    
    isServerRunning_ = false;
}

double WebSocketServer::getThroughput() const {
    if (processingMessagesReceived_ == 0) return 0.0;
    
    // Use processingEndTime_ if available; otherwise current time
    auto endTimeToUse = (processingEndTime_ > processingStartTime_) ? processingEndTime_ : std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - processingStartTime_);
    
    if (duration.count() == 0) return 0.0;
    
    return (double)processingMessagesReceived_ * 1000.0 / duration.count();
}

double WebSocketServer::getAverageOrderProcessNs() const {
    if (processingTimingSamples_ == 0) return 0.0;
    return static_cast<double>(processingTotalTimeNs_) / static_cast<double>(processingTimingSamples_);
}

uint64_t WebSocketServer::getP99OrderProcessNs() const {
    if (processingTimingReservoir_.empty()) return 0;
    const size_t sampleCount = static_cast<size_t>(std::min<uint64_t>(processingTimingSamples_, processingTimingReservoir_.size()));
    if (sampleCount == 0) return 0;
    std::vector<uint64_t> samples;
    samples.reserve(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        samples.push_back(processingTimingReservoir_[i]);
    }
    size_t idx = (sampleCount * 99 + 99) / 100; // ceil(0.99 * n)
    if (idx == 0) idx = 1;
    if (idx > sampleCount) idx = sampleCount;
    std::nth_element(samples.begin(), samples.begin() + (idx - 1), samples.end());
    return samples[idx - 1];
}

