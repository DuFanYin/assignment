#include "project/server.hpp"
#include "project/utils.hpp"
#include "project/config.hpp"
#include "database/clickhouse_connection.hpp"
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
#include <unistd.h>  // for unlink
#include <fcntl.h>   // for O_TMPFILE (if available)
// Use JSON from databento dependencies
#include <nlohmann/json.hpp>

// Include uWebSockets
#include "src/App.h"

namespace db = databento;

WebSocketServer::WebSocketServer(int port, const ClickHouseConnection::Config& dbConfig,
                                  size_t topLevels,
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
    , processingMessagesReceived_(0)
    , processingOrdersProcessed_(0)
    , processingTotalTimeNs_(0)
    , processingTimingSamples_(0)
    , processingTimingReservoir_()
    , processingRng_(std::random_device{}())
{
    // Initialize order book (symbol will be set from DBN file metadata)
    orderBook_->setTopLevels(topLevels_);
    
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
    
    // Note: Database writer thread is started per-cycle in startProcessingThread()
    // We don't start it here to allow multiple cycles
    
    // Start uWebSockets server
    // DISABLE compression for binary data - DBN files are already compressed, compression adds CPU overhead
    uWS::App().ws<WebSocketServer::PerSocketData>("/*", {
        .compression = uWS::CompressOptions(uWS::DISABLED),  // Disabled for speed - binary data already compressed
        .maxPayloadLength = kMaxPayloadLength,
        .idleTimeout = 16,
        .maxBackpressure = kMaxPayloadLength,
        .closeOnBackpressureLimit = false,
        .resetIdleTimeoutOnSend = false,
        .sendPingsAutomatically = true,
        .upgrade = nullptr,
        .open = [this](auto *ws) {
            auto *data = ws->getUserData();
            data->totalBytesReceived = 0;
            data->bytesReceived = 0;
            data->isMetadataReceived = false;
            data->fileName.clear();
            data->fileSize = 0;
            data->isProcessingStarted = false;
            data->dbnBuffer.clear();
            data->lastProgressUpdate = 0;
            
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
            
            if (opCode == uWS::OpCode::BINARY) {
                // Check if this is binary metadata (first byte = 'M')
                if (!data->isMetadataReceived && message.size() >= 5 && 
                    static_cast<uint8_t>(message[0]) == 0x4D) {
                    // Parse binary metadata: [1 byte 'M'][4 bytes fileSize][N bytes fileName]
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(message.data());
                    data->fileSize = (static_cast<uint32_t>(bytes[1]) << 24) |
                                    (static_cast<uint32_t>(bytes[2]) << 16) |
                                    (static_cast<uint32_t>(bytes[3]) << 8) |
                                    static_cast<uint32_t>(bytes[4]);
                    data->fileName = std::string(message.data() + 5, message.size() - 5);
                    data->isMetadataReceived = true;
                    
                    // Mark upload start time
                    uploadStartTime_ = std::chrono::steady_clock::now();
                    uploadEndTime_ = {};
                    
                    // Pre-allocate in-memory buffer (no temp file!)
                    data->dbnBuffer.clear();
                    data->dbnBuffer.reserve(data->fileSize);
                    data->bytesReceived = 0;
                    data->lastProgressUpdate = 0;
                    
                    // No response needed - client starts sending immediately
                    return;
                }
                
                // Handle binary DBN data
                if (!data->isMetadataReceived) {
                    nlohmann::json error;
                    error["type"] = "error";
                    error["error"] = "Metadata must be sent first";
                    ws->send(error.dump(), uWS::OpCode::TEXT, false);
                    return;
                }
                
                // Append chunk to in-memory buffer (optimized - reserve space to avoid reallocations)
                // Check if we need to grow the buffer
                if (data->dbnBuffer.size() + message.size() > data->dbnBuffer.capacity()) {
                    // Grow by at least 50% or enough for this message
                    size_t newCapacity = std::max(
                        data->dbnBuffer.capacity() * 3 / 2,
                        data->dbnBuffer.size() + message.size()
                    );
                    data->dbnBuffer.reserve(newCapacity);
                }
                // Direct append (faster than insert)
                data->dbnBuffer.insert(data->dbnBuffer.end(), message.begin(), message.end());
                data->bytesReceived += message.size();
                totalBytesReceived_ += message.size();
                
                // No progress updates during upload (reduces WebSocket overhead)
                // Frontend can calculate progress locally based on chunks sent
                
                // Check if file upload is complete and automatically start processing
                if (!data->isProcessingStarted && data->bytesReceived >= data->fileSize && data->fileSize > 0) {
                    // Mark upload end time
                    uploadEndTime_ = std::chrono::steady_clock::now();
                    data->isProcessingStarted = true;  // Mark as started to prevent duplicate processing
                    
                    // Send status update
                    nlohmann::json status;
                    status["type"] = "stats";
                    status["status"] = "Processing file...";
                    status["messagesProcessed"] = 0;
                    ws->send(status.dump(), uWS::OpCode::TEXT, false);
                    
                    // Start processing (move buffer ownership to processing thread)
                    startProcessingThread(std::move(data->dbnBuffer), data->sendMessage);
                }
            }
        },
        .close = [this](auto *ws, int code, std::string_view message) {
            (void)code;
            (void)message;
            auto *data = ws->getUserData();
            
            // Safety fallback: Process any remaining data if not already processing
            // (This should rarely trigger since processing auto-starts when upload completes)
            if (!data->isProcessingStarted && !data->dbnBuffer.empty() && data->fileSize > 0) {
                data->isProcessingStarted = true;  // Mark as started to prevent duplicate processing
                startProcessingThread(std::move(data->dbnBuffer), data->sendMessage);
            }
            
            // No temp file cleanup needed - all in memory!
        }
    }).get("/status/:session_id", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) noexcept {
        // Check if session is complete (ClickHouse)
        res->writeHeader("Content-Type", "application/json");
        res->writeHeader("Access-Control-Allow-Origin", "*");
        
        std::string sessionId = std::string(req->getParameter(0));
        
        if (sessionId.empty() || !jsonGenerator_) {
            res->writeStatus("400 Bad Request");
            res->end("{\"error\":\"Invalid session ID\"}");
            return;
        }
        
        nlohmann::json response;
        try {
            auto* client = jsonGenerator_->getConnection().getClient();
            if (!client) {
                res->writeStatus("500 Internal Server Error");
                res->end("{\"error\":\"DB client not available\"}");
                return;
            }
            std::string query = "SELECT status FROM processing_sessions WHERE session_id = '" + sessionId + "' LIMIT 1";
            std::string status;
            client->Select(query, [&status](const clickhouse::Block& block) {
                if (block.GetRowCount() > 0) {
                    auto col = block[0]->As<clickhouse::ColumnString>();
                    status = col->At(0);
                }
            });
            if (!status.empty()) {
                response["sessionId"] = sessionId;
                response["status"] = status;
                response["complete"] = (status == "completed");
            } else {
                response["error"] = "Session not found";
                response["complete"] = false;
            }
            res->writeStatus("200 OK");
            res->end(response.dump());
        } catch (const std::exception& e) {
            res->writeStatus("500 Internal Server Error");
            res->end(std::string("{\"error\":\"") + e.what() + "\"}");
        }
    }).get("/download/json", [this](auto *res, auto *req) noexcept {
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

void WebSocketServer::processDbnFromMemory(const std::vector<uint8_t>& dbnData,
                                           const std::function<void(const std::string&)>& sendMessage) {
    if (dbnData.empty()) {
        return;
    }
    
    try {
        // Create temporary file in /tmp (often tmpfs/RAM on Linux, fast on macOS)
        // Write data, then parse immediately - OS will keep it in page cache
        std::filesystem::path tempDir = std::filesystem::temp_directory_path();
        std::string tempPath = (tempDir / ("dbn_processing_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dbn")).string();
        
        // Write buffer to temp file (one-time write, no chunk-by-chunk I/O)
        {
            std::ofstream tempFile(tempPath, std::ios::binary);
            if (!tempFile.is_open()) {
                throw std::runtime_error("Failed to create temporary processing file");
            }
            tempFile.write(reinterpret_cast<const char*>(dbnData.data()), dbnData.size());
            tempFile.close();
        }
        
        // Use DbnFileStore to parse the file (reads from OS page cache, effectively memory)
        db::DbnFileStore store(tempPath);
        
        // Unlink immediately after opening - file stays in memory via open file descriptor
        // This eliminates disk I/O after initial write (which is in page cache anyway)
        unlink(tempPath.c_str());
        
        // Extract symbol from DBN file metadata
        auto metadata = store.GetMetadata();
        if (!metadata.symbols.empty()) {
            symbol_ = metadata.symbols[0];  // Use first symbol from the file
            orderBook_->setSymbol(symbol_);
        } else {
            utils::logWarning("No symbols found in DBN file metadata");
        }
        
        // Start database session before processing
        if (databaseWriter_ && !symbol_.empty()) {
            try {
                databaseWriter_->startSession(symbol_, "upload.dbn", dbnData.size());
                activeSessionId_ = databaseWriter_->getCurrentSessionId();  // Cache it - never touch databaseWriter_ again from this thread
            } catch (const std::exception& e) {
                utils::logError("Failed to start database session: " + std::string(e.what()));
            }
        }
        
        // Reset statistics for this processing session
        processingMessagesReceived_ = 0;
        processingOrdersProcessed_ = 0;
        processingTotalTimeNs_ = 0;
        processingTimingSamples_ = 0;
        processingTimingReservoir_.clear();
        
        // Start timing when parsing begins (first message taken from DBN)
        processingStartTime_ = std::chrono::steady_clock::now();
        
        // Process all records
        const db::Record* record;
        while ((record = store.NextRecord()) != nullptr) {
            if (record->RType() == db::RType::Mbo) {
                const auto& mbo = record->Get<db::MboMsg>();
                
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
                    
                    // Update timing reservoir
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
                    
                    // Send periodic status updates
                    if (sendMessage && totalMessagesProcessed_ % kStatusUpdateInterval == 0) {
                        nlohmann::json stats;
                        stats["type"] = "stats";
                        stats["status"] = "Processing...";
                        stats["messagesProcessed"] = totalMessagesProcessed_.load();
                        sendMessage(stats.dump());
                    }
                } catch (const std::invalid_argument& e) {
                    // Handle missing orders/levels gracefully
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
        // Update total throughput now that DB processing has completed
        sessionStats_.throughput = getThroughput();
        
        if (sendMessage) {
            nlohmann::json complete;
            complete["type"] = "complete";
            complete["messagesReceived"] = processingMessagesReceived_.load();
            complete["ordersProcessed"] = processingOrdersProcessed_.load();
            complete["messagesProcessed"] = totalMessagesProcessed_.load();
            complete["bytesReceived"] = totalBytesReceived_.load();
            complete["dbWritesPending"] = snapshotRingBuffer_->size();
            complete["sessionId"] = activeSessionId_;
            // Compute durations on the server side
            auto zero_tp = std::chrono::steady_clock::time_point{};
            // Total duration: from metadata arrival to DB thread completion
            double totalDurationSec = 0.0;
            if (uploadStartTime_ > zero_tp && dbEndTime_ > zero_tp && dbEndTime_ > uploadStartTime_) {
                totalDurationSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - uploadStartTime_).count()) / 1000.0;
            }
            double uploadDurationSec = 0.0;
            if (uploadEndTime_ > uploadStartTime_ && uploadStartTime_ > zero_tp) {
                uploadDurationSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(uploadEndTime_ - uploadStartTime_).count()) / 1000.0;
            }
            double processingDurationSec = 0.0;
            if (processingEndTime_ > processingStartTime_) {
                processingDurationSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(processingEndTime_ - processingStartTime_).count()) / 1000.0;
            }
            double dbDurationSec = 0.0;
            if (dbEndTime_ > dbStartTime_ && dbStartTime_ > zero_tp) {
                dbDurationSec = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - dbStartTime_).count()) / 1000.0;
            }

            double totalThroughput = getThroughput();
            complete["totalThroughput"] = totalThroughput;
            complete["totalDurationSec"] = totalDurationSec;
            double orderThroughput = getOrderThroughput();
            complete["orderThroughput"] = orderThroughput;
            complete["processingDurationSec"] = processingDurationSec;
            complete["dbThroughput"] = getDbThroughput();
            complete["dbDurationSec"] = dbDurationSec;
            complete["uploadThroughputMsgs"] = getUploadThroughputMsgs();
            complete["uploadDurationSec"] = uploadDurationSec;
            
            // Calculate order processing statistics
            double avgNs = getAverageOrderProcessNs();
            uint64_t p99Ns = getP99OrderProcessNs();
            if (avgNs > 0.0) {
                complete["averageOrderProcessNs"] = avgNs;
                complete["p99OrderProcessNs"] = p99Ns;
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
                    double bidValue = static_cast<double>(finalBid.price) / static_cast<double>(kPriceScaleFactor);
                    double askValue = static_cast<double>(finalAsk.price) / static_cast<double>(kPriceScaleFactor);
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
        
        // Capture final book state for DB thread to write
        if (orderBook_) {
            auto finalBbo = orderBook_->Bbo();
            auto finalBid = finalBbo.first;
            auto finalAsk = finalBbo.second;
            
            if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
                sessionStats_.totalOrders = orderBook_->GetOrderCount();
                sessionStats_.bidLevels = orderBook_->GetBidLevelCount();
                sessionStats_.askLevels = orderBook_->GetAskLevelCount();
                sessionStats_.bestBid = static_cast<double>(finalBid.price) / static_cast<double>(kPriceScaleFactor);
                sessionStats_.bestAsk = static_cast<double>(finalAsk.price) / static_cast<double>(kPriceScaleFactor);
                sessionStats_.spread = std::abs(sessionStats_.bestAsk - sessionStats_.bestBid);
                sessionStats_.hasBookState = true;
                
                // Memory fence: ensure all writes are visible
                std::atomic_thread_fence(std::memory_order_release);
            }
        }
        
        // DB thread will write stats and end session when it finishes
        if (orderBook_) {
            orderBook_->Clear();
        }
        
        // No cleanup needed - file was already unlinked and only existed in memory
        
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
    constexpr size_t kBatchSize = 50000;  // Larger batches for COPY BINARY
    std::vector<MboMessageWrapper> batch;
    batch.reserve(kBatchSize);
    bool started = false;
    
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
            if (!started) {
                started = true;
                dbStartTime_ = std::chrono::steady_clock::now();
            }
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
            std::this_thread::sleep_for(kDatabaseWriterSleepMs);
        }
    }
    
    // Final flush of any remaining items
    writeBatch();
    dbEndTime_ = std::chrono::steady_clock::now();
    // Compute DB throughput over DB writer active window
    if (started && dbEndTime_ > dbStartTime_) {
        auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - dbStartTime_).count();
        if (durMs > 0) {
            dbThroughput_ = static_cast<double>(itemsWritten) * 1000.0 / static_cast<double>(durMs);
        } else {
            dbThroughput_ = 0.0;
        }
    } else {
        dbThroughput_ = 0.0;
    }
    
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
    
    // Wait for processing thread to complete
    if (processingThread_.has_value() && processingThread_->joinable()) {
        processingThread_->join();
    }
    processingThread_.reset();
    
    // Wait for database writer thread to complete
    if (databaseWriterThread_.joinable()) {
        databaseWriterThread_.join();
    }
}

double WebSocketServer::getThroughput() const {
    // Total throughput from metadata arrival through DB thread completion.
    if (processingMessagesReceived_ == 0) return 0.0;
    auto zero_tp = std::chrono::steady_clock::time_point{};
    // Must start from metadata arrival (uploadStartTime_)
    if (uploadStartTime_ <= zero_tp) return 0.0;
    // Must end when DB thread finishes (dbEndTime_)
    if (dbEndTime_ <= zero_tp || dbEndTime_ <= uploadStartTime_) return 0.0;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - uploadStartTime_);
    if (duration.count() == 0) return 0.0;
    return static_cast<double>(processingMessagesReceived_) * 1000.0 / static_cast<double>(duration.count());
}

// removed: getProcessingThroughput

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

double WebSocketServer::getOrderThroughput() const {
    // Orders per second over the processing window only
    if (processingOrdersProcessed_ == 0) return 0.0;
    if (!(processingEndTime_ > processingStartTime_)) return 0.0;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(processingEndTime_ - processingStartTime_);
    if (duration.count() == 0) return 0.0;
    return static_cast<double>(processingOrdersProcessed_) * 1000.0 / static_cast<double>(duration.count());
}

double WebSocketServer::getUploadThroughputMsgs() const {
    // Messages per second between uploadStartTime_ and uploadEndTime_
    // Use processingMessagesReceived_ (total decoded/processed messages) as the numerator
    auto zero_tp = std::chrono::steady_clock::time_point{};
    if (!(uploadEndTime_ > uploadStartTime_) || uploadStartTime_ == zero_tp) return 0.0;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(uploadEndTime_ - uploadStartTime_);
    if (duration.count() == 0) return 0.0;
    const double msgs = static_cast<double>(processingMessagesReceived_.load(std::memory_order_acquire));
    if (msgs <= 0.0) return 0.0;
    return msgs * 1000.0 / static_cast<double>(duration.count());
}

// removed: getUploadTimePerMsgMs

void WebSocketServer::startProcessingThread(std::vector<uint8_t>&& dbnData, const std::function<void(const std::string&)>& sendMessage) {
    // Once processing starts, it cannot be stopped - runs to completion independently of WebSocket connection
    if (processingThread_.has_value() && processingThread_->joinable()) {
        processingThread_->join();
    }
    processingThread_.reset();
    if (databaseWriterThread_.joinable()) {
        databaseWriterThread_.join();
    }
    isServerRunning_.store(true, std::memory_order_release);
    dbThroughput_ = 0.0;
    dbStartTime_ = {};
    dbEndTime_ = {};
    if (orderBook_) {
        orderBook_->Clear();
    }
    databaseWriterThread_ = std::jthread([this](std::stop_token st) {
        this->databaseWriterLoop(st);
    });
    // No delay needed - ring buffer handles synchronization automatically
    
    processingThread_ = std::jthread([this, data = std::move(dbnData), sendMessage](std::stop_token) mutable {
        processDbnFromMemory(data, sendMessage);
    });
}

