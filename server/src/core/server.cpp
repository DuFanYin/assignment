#include "core/server.hpp"
#include "util/streamer.hpp"
#include "database/json_generator.hpp"
#include "util/utils.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

// Include uWebSockets
#include "App.h"
#include "Loop.h"

WebSocketServer::WebSocketServer(int port, const ClickHouseConnection::Config& dbConfig,
                                 size_t topLevels)
    : port_(port)
    , databaseConfig_(dbConfig)
    , isServerRunning_(false)
    , totalBytesReceived_(0)
    , topLevels_(topLevels)
    , persistenceManager_(std::make_unique<PersistenceManager>(databaseConfig_))
    , processingManager_(std::make_unique<ProcessingManager>(topLevels_, isServerRunning_)) {
    processingManager_->attachPersistence(persistenceManager_.get());
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start() {
    if (!persistenceManager_->initialize()) {
        utils::logError("Failed to initialize persistence layer");
        return false;
    }

    // Start uWebSockets server - compression disabled (DBN files already compressed)
    uWS::App().ws<WebSocketServer::PerSocketData>("/*", {
        .compression = uWS::CompressOptions(uWS::DISABLED),
        .maxPayloadLength = kMaxPayloadLength,
        .idleTimeout = 16,
        .maxBackpressure = kMaxPayloadLength,
        .closeOnBackpressureLimit = false,
        .resetIdleTimeoutOnSend = false,
        .sendPingsAutomatically = true,
        .upgrade = nullptr,
        .open = [](auto *ws) {
            auto *data = ws->getUserData();
            data->totalBytesReceived = 0;
            data->bytesReceived = 0;
            data->isMetadataReceived = false;
            data->fileName.clear();
            data->fileSize = 0;
            data->isProcessingStarted = false;
            data->streamBuffer.reset();
            
            // Thread-safe send callback: capture loop from event loop thread, defer sends to event loop
            auto* loop = uWS::Loop::get();
            data->sendMessage = [ws, loop](const std::string& message) {
                if (!loop) return;
                loop->defer([ws, msg = std::string(message)]() {
                    ws->send(msg, uWS::OpCode::TEXT, false);
                });
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
                    uploadBytesReceived_ = 0;  // Reset for new session
                    
                    // Initialize streaming buffer
                    data->streamBuffer = std::make_shared<StreamBuffer>();
                    data->bytesReceived = 0;
                    
                    // Start processing immediately - processing thread reads from buffer as chunks arrive
                    data->isProcessingStarted = true;
                    processingManager_->setUploadMetrics(uploadStartTime_, uploadEndTime_, uploadBytesReceived_);
                    processingManager_->startProcessing(
                        data->streamBuffer,
                        data->fileSize,
                        data->fileName,
                        data->sendMessage
                    );
                    
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
                
                if (!data->streamBuffer) {
                    data->streamBuffer = std::make_shared<StreamBuffer>();
                }
                
                // Append chunk to streaming buffer - processing thread is already running and will consume it
                data->streamBuffer->appendChunk(reinterpret_cast<const uint8_t*>(message.data()), message.size());
                data->bytesReceived += message.size();
                uploadBytesReceived_ += message.size();  // Track bytes for this session
                totalBytesReceived_ += message.size();
                
                // No progress updates during upload - frontend calculates progress locally
                
                // File upload complete - signal end of stream to processing thread
                if (data->bytesReceived >= data->fileSize && data->fileSize > 0) {
                    uploadEndTime_ = std::chrono::steady_clock::now();
                    processingManager_->setUploadMetrics(uploadStartTime_, uploadEndTime_, uploadBytesReceived_);
                    if (data->streamBuffer) {
                        data->streamBuffer->markFinished();
                    }
                }
            }
        },
        .close = [this](auto *ws, int code, std::string_view message) {
            (void)code;
            (void)message;
            auto *data = ws->getUserData();
            
            // Safety fallback: start processing if not started, signal end of stream if upload incomplete
            if (!data->isProcessingStarted && data->streamBuffer && data->fileSize > 0) {
                data->isProcessingStarted = true;
                uploadEndTime_ = std::chrono::steady_clock::now();
                data->streamBuffer->markFinished();
                processingManager_->setUploadMetrics(uploadStartTime_, uploadEndTime_, uploadBytesReceived_);
                processingManager_->startProcessing(
                    data->streamBuffer,
                    data->fileSize,
                    data->fileName,
                    data->sendMessage
                );
            } else if (data->streamBuffer) {
                // Processing already started - just signal end of stream
                uploadEndTime_ = std::chrono::steady_clock::now();
                processingManager_->setUploadMetrics(uploadStartTime_, uploadEndTime_, uploadBytesReceived_);
                data->streamBuffer->markFinished();
            }
            
            // No temp file cleanup needed - all in memory!
        }
    }).get("/status/:session_id", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) noexcept {
        // Check if session is complete (ClickHouse)
        res->writeHeader("Content-Type", "application/json");
        res->writeHeader("Access-Control-Allow-Origin", "*");
        
        std::string sessionId = std::string(req->getParameter(0));
        
        auto* jsonGenerator = persistenceManager_->jsonGenerator();
        if (sessionId.empty() || !jsonGenerator) {
            res->writeStatus("400 Bad Request");
            res->end("{\"error\":\"Invalid session ID\"}");
            return;
        }
        
        nlohmann::json response;
        try {
            auto* client = jsonGenerator->getConnection().getClient();
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
        auto* jsonGenerator = persistenceManager_->jsonGenerator();
        if (!sessionId.empty() && jsonGenerator) {
            jsonData = jsonGenerator->generateJSON(sessionId);
        } else if (jsonGenerator && !processingManager_->symbol().empty()) {
            jsonData = jsonGenerator->generateJSONForSymbol(processingManager_->symbol());
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

void WebSocketServer::stop() {
    processingManager_->stopProcessing();
    persistenceManager_->markProcessingComplete();
    persistenceManager_->waitForCompletion();
}

