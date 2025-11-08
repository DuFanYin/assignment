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
    , dbConfig_(dbConfig)
    , running_(false)
    , messagesProcessed_(0)
    , bytesReceived_(0)
    , orderBook_(std::make_unique<Book>())
    , jsonRingBuffer_(std::make_unique<RingBuffer<MboMessageWrapper>>(ringBufferSize))
    , symbol_("")  // Will be extracted from DBN file
    , topLevels_(topLevels)
    , outputFullBook_(outputFullBook)
    , receivedMessages_(0)
    , processedOrders_(0)
    , totalProcessTimeNs_(0)
    , timingSamples_(0)
    , timingReservoir_()
    , rng_(std::random_device{}())
{
    // Initialize order book (symbol will be set from DBN file metadata)
    orderBook_->setTopLevels(topLevels_);
    orderBook_->setOutputFullBook(outputFullBook_);
    
    // Initialize timing reservoir
    timingReservoir_.reserve(kTimingReservoirSize);
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start() {
    if (running_) {
        return false;
    }
    
    // Initialize database writer and JSON generator
    try {
        dbWriter_ = std::make_unique<project::DatabaseWriter>(dbConfig_);
        jsonGenerator_ = std::make_unique<project::JSONGenerator>(dbConfig_);
        std::cout << "Database writer initialized" << std::endl;
    } catch (const std::exception& e) {
        utils::logError("Failed to initialize database writer: " + std::string(e.what()));
        return false;
    }
    
    // Set running_ to true BEFORE starting the thread to avoid race condition
    running_ = true;
    std::cout << "Server started, running_ = true" << std::endl;
    
    // Start database writer thread
    std::cout << "Starting database writer thread..." << std::endl;
    dbWriterThread_ = std::thread([this]() {
        this->databaseWriterLoop();
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
            data->bytesReceived = 0;
            data->metadataReceived = false;
            data->fileName.clear();
            data->fileSize = 0;
            data->processingStarted = false;
            
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
                        data->metadataReceived = true;
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
                        if (!data->processingStarted && !data->tempFilePath.empty() && std::filesystem::exists(data->tempFilePath)) {
                            data->processingStarted = true;
                            
                            // Send status update
                            nlohmann::json status;
                            status["type"] = "stats";
                            status["status"] = "Processing file...";
                            status["messagesProcessed"] = 0;
                            ws->send(status.dump(), uWS::OpCode::TEXT, false);
                            
                            // Process in a separate thread to avoid blocking
                            std::string tempPath = data->tempFilePath;
                            std::function<void(const std::string&)> sendMsg = data->sendMessage;
                            std::thread([this, tempPath, sendMsg]() {
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
                if (!data->metadataReceived) {
                    nlohmann::json error;
                    error["type"] = "error";
                    error["error"] = "Metadata must be sent first";
                    ws->send(error.dump(), uWS::OpCode::TEXT, false);
                    return;
                }
                
                // Append to buffer and write to temp file
                std::vector<uint8_t> chunk(message.begin(), message.end());
                data->buffer.insert(data->buffer.end(), chunk.begin(), chunk.end());
                bytesReceived_ += chunk.size();
                
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
                if (!data->processingStarted && data->buffer.size() >= data->fileSize && data->fileSize > 0) {
                    data->processingStarted = true;
                    
                    // Send status update
                    nlohmann::json status;
                    status["type"] = "stats";
                    status["status"] = "Processing file...";
                    status["messagesProcessed"] = 0;
                    ws->send(status.dump(), uWS::OpCode::TEXT, false);
                    
                    // Process the complete DBN file in a separate thread
                    std::string tempPath = data->tempFilePath;
                    std::function<void(const std::string&)> sendMsg = data->sendMessage;
                    std::thread([this, tempPath, sendMsg]() {
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
            if (!data->processingStarted && !data->tempFilePath.empty() && std::filesystem::exists(data->tempFilePath)) {
                data->processingStarted = true;
                std::string tempPath = data->tempFilePath;
                std::function<void(const std::string&)> sendMsg = data->sendMessage;
                std::thread([this, tempPath, sendMsg]() {
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
        if (listen_socket) {
            std::cout << "WebSocket server listening on port " << port_ << std::endl;
            std::cout << "HTTP server available at http://localhost:" << port_ << std::endl;
        } else {
            std::cerr << "Failed to listen on port " << port_ << std::endl;
            running_ = false;
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
        std::cout << "Starting to process DBN file: " << socketData->tempFilePath << std::endl;
        db::DbnFileStore store(socketData->tempFilePath);
        
        // Extract symbol from DBN file metadata
        auto metadata = store.GetMetadata();
        if (!metadata.symbols.empty()) {
            symbol_ = metadata.symbols[0];  // Use first symbol from the file
            orderBook_->setSymbol(symbol_);
            
            std::cout << "Extracted symbol from DBN file: " << symbol_ << std::endl;
            
            if (sendMessage) {
                nlohmann::json info;
                info["type"] = "info";
                info["message"] = "Extracted symbol from DBN file: " + symbol_;
                sendMessage(info.dump());
            }
        } else {
            utils::logWarning("No symbols found in DBN file metadata");
            std::cerr << "WARNING: No symbols found in DBN file metadata" << std::endl;
        }
        
        // Start timing when processing begins (from pressing start button)
        startTime_ = std::chrono::steady_clock::now();
        
        // Reset statistics for this processing session
        receivedMessages_ = 0;
        processedOrders_ = 0;
        totalProcessTimeNs_ = 0;
        timingSamples_ = 0;
        timingReservoir_.clear();
        
        std::cout << "Starting to process records..." << std::endl;
        
        // Process all records
        const db::Record* record;
        bool timingStarted = false;
        while ((record = store.NextRecord()) != nullptr) {
            if (record->RType() == db::RType::Mbo) {
                const auto& mbo = record->Get<db::MboMsg>();
                
                // Start timing on first processed message (same as receiver version)
                if (!timingStarted) {
                    startTime_ = std::chrono::steady_clock::now();
                    timingStarted = true;
                }
                
                // Count message as received
                receivedMessages_++;
                
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
                    totalProcessTimeNs_ += elapsedNs;
                    ++timingSamples_;
                    
                    // Update timing reservoir (same as receiver version)
                    if (timingReservoir_.size() < kTimingReservoirSize) {
                        timingReservoir_.push_back(elapsedNs);
                    } else {
                        std::uniform_int_distribution<uint64_t> dist(0, timingSamples_ - 1);
                        const uint64_t idx = dist(rng_);
                        if (idx < kTimingReservoirSize) {
                            timingReservoir_[static_cast<size_t>(idx)] = elapsedNs;
                        }
                    }
                    processedOrders_++;
                    
                    // Push to ring buffer for JSON generation
                    MboMessageWrapper wrapper(snap);
                    jsonRingBuffer_->push(wrapper);
                    
                    // Debug: log every 1000 messages
                    if (messagesProcessed_ % 1000 == 0) {
                        std::cout << "Pushed " << messagesProcessed_ << " messages to ring buffer" << std::endl;
                    }
                    
                    messagesProcessed_++;
                    
                    // Send periodic status updates (every 1000 messages)
                    if (sendMessage && messagesProcessed_ % 1000 == 0) {
                        nlohmann::json stats;
                        stats["type"] = "stats";
                        stats["status"] = "Processing...";
                        stats["messagesProcessed"] = messagesProcessed_.load();
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
        
        std::cout << "Finished processing records. Total messages: " << messagesProcessed_ << std::endl;
        std::cout << "Database writes continue in background..." << std::endl;
        
        // End timing when processing completes
        endTime_ = std::chrono::steady_clock::now();
        
        // Update session statistics in database
        if (dbWriter_) {
            double throughput = getThroughput();
            double avgNs = getAverageOrderProcessNs();
            uint64_t p99Ns = getP99OrderProcessNs();
            
            dbWriter_->updateSessionStats(
                receivedMessages_.load(),
                processedOrders_.load(),
                throughput,
                static_cast<int64_t>(avgNs),
                p99Ns
            );
        }
        
        // Send completion message with statistics (same format as receiver version)
        if (sendMessage) {
            
            nlohmann::json complete;
            complete["type"] = "complete";
            complete["messagesReceived"] = receivedMessages_.load();
            complete["ordersProcessed"] = processedOrders_.load();
            complete["messagesProcessed"] = messagesProcessed_.load();
            complete["bytesReceived"] = bytesReceived_.load();
            if (dbWriter_) {
                complete["sessionId"] = dbWriter_->getCurrentSessionId();
            }
            
            // Calculate throughput (same as receiver version)
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
            
            sendMessage(complete.dump());
        }
        
        // Update final book state in database
        if (orderBook_ && dbWriter_) {
            auto finalBbo = orderBook_->Bbo();
            auto finalBid = finalBbo.first;
            auto finalAsk = finalBbo.second;
            
            if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
                double bidValue = static_cast<double>(finalBid.price) / 1000000000.0;
                double askValue = static_cast<double>(finalAsk.price) / 1000000000.0;
                double spreadValue = std::abs(askValue - bidValue);
                
                dbWriter_->updateFinalBookState(
                    orderBook_->GetOrderCount(),
                    orderBook_->GetBidLevelCount(),
                    orderBook_->GetAskLevelCount(),
                    bidValue,
                    askValue,
                    spreadValue
                );
            }
        }
        
        // End database session
        if (dbWriter_) {
            dbWriter_->endSession(true);
        }
        
        // Clean up order book after processing completes
        if (orderBook_) {
            orderBook_->Clear();
            std::cout << "Order book cleared after processing" << std::endl;
        }
        
        
    } catch (const std::exception& e) {
        utils::logError("Error processing DBN file: " + std::string(e.what()));
        
        // End database session with error
        if (dbWriter_) {
            dbWriter_->endSession(false, e.what());
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

void WebSocketServer::processDbnRecord(const uint8_t* /*data*/, size_t /*size*/) {
    // Not used - processing is done in processDbnChunk
}

void WebSocketServer::databaseWriterLoop() {
    MboMessageWrapper wrapper;
    
    std::cout << "Database writer thread started" << std::endl;
    
    // Continue processing until stopped AND ring buffer is empty
    while (running_.load() || !jsonRingBuffer_->empty()) {
        if (jsonRingBuffer_->try_pop(wrapper)) {
            // Successfully popped an item
            try {
                // Check if database writer is available
                if (!dbWriter_) {
                    utils::logError("Database writer is null!");
                    continue;
                }
                
                // Write snapshot to database
                if (!dbWriter_->writeSnapshot(wrapper.snapshot)) {
                    utils::logError("Failed to write snapshot to database");
                }
            } catch (const std::exception& e) {
                utils::logError("Error writing to database: " + std::string(e.what()));
            }
        } else {
            // Buffer is empty - wait for data
            jsonRingBuffer_->wait_for_data();
        }
    }
    
    std::cout << "Database writer thread exiting." << std::endl;
}

void WebSocketServer::stop() {
    if (!running_) {
        return;
    }
    
    std::cout << "Stopping server..." << std::endl;
    running_ = false;
    
    // Stop database writer thread (wait for it to finish processing remaining items)
    if (dbWriterThread_.joinable()) {
        std::cout << "Waiting for database writer thread to finish..." << std::endl;
        jsonRingBuffer_->notify_all(); // Wake up blocking pop
        dbWriterThread_.join();
        std::cout << "Database writer thread stopped." << std::endl;
    }
    
    // Close database writer
    if (dbWriter_) {
        std::cout << "Closing database writer..." << std::endl;
        dbWriter_.reset();
        std::cout << "Database writer closed." << std::endl;
    }
    
    std::cout << "Server stopped." << std::endl;
}

double WebSocketServer::getThroughput() const {
    if (receivedMessages_ == 0) return 0.0;
    
    // Use endTime_ if available; otherwise current time
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - startTime_);
    
    if (duration.count() == 0) return 0.0;
    
    return (double)receivedMessages_ * 1000.0 / duration.count();
}

double WebSocketServer::getAverageOrderProcessNs() const {
    if (timingSamples_ == 0) return 0.0;
    return static_cast<double>(totalProcessTimeNs_) / static_cast<double>(timingSamples_);
}

uint64_t WebSocketServer::getP99OrderProcessNs() const {
    if (timingReservoir_.empty()) return 0;
    const size_t sampleCount = static_cast<size_t>(std::min<uint64_t>(timingSamples_, timingReservoir_.size()));
    if (sampleCount == 0) return 0;
    std::vector<uint64_t> samples;
    samples.reserve(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        samples.push_back(timingReservoir_[i]);
    }
    size_t idx = (sampleCount * 99 + 99) / 100; // ceil(0.99 * n)
    if (idx == 0) idx = 1;
    if (idx > sampleCount) idx = sampleCount;
    std::nth_element(samples.begin(), samples.begin() + (idx - 1), samples.end());
    return samples[idx - 1];
}

