#pragma once

#include <string>
#include <memory>
#include <thread>
#include <stop_token>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include <random>
#include <optional>
#include <fstream>
#include "core/processing_manager.hpp"
#include "core/persistence_manager.hpp"
#include "util/utils.hpp"

// Forward declaration
class StreamBuffer;

// Include ClickHouseConnection for Config
#include "database/clickhouse_connection.hpp"

class WebSocketServer {
public:
    WebSocketServer(int port, const ClickHouseConnection::Config& dbConfig,
                    size_t topLevels = 10);
    ~WebSocketServer();
    
    bool start();
    void stop();
    bool isRunning() const { return isServerRunning_; }
    
    // Statistics
    size_t getMessagesProcessed() const {
        return processingManager_ ? processingManager_->totalMessagesProcessed() : 0;
    }
    size_t getBytesReceived() const { return totalBytesReceived_; }
    
    struct PerSocketData {
        // Upload state
        size_t totalBytesReceived = 0;      // global counter across sockets (stats)
        size_t bytesReceived = 0;            // per-connection bytes received
        bool isMetadataReceived = false;
        std::string fileName;
        size_t fileSize = 0;
        bool isProcessingStarted = false;
        std::function<void(const std::string&)> sendMessage; // Callback to send messages (thread-safe via loop->defer)
        
        // Streaming buffer for incremental decoding
        std::shared_ptr<StreamBuffer> streamBuffer;
    };
    
private:
    int port_;
    ClickHouseConnection::Config databaseConfig_;
    std::atomic<bool> isServerRunning_;
    std::atomic<size_t> totalBytesReceived_;
    
    size_t topLevels_;
    
    std::chrono::steady_clock::time_point uploadStartTime_;
    std::chrono::steady_clock::time_point uploadEndTime_;
    size_t uploadBytesReceived_{0};  // Bytes received during current upload session
    
    // Constants
    static constexpr size_t kMaxPayloadLength = 100 * 1024 * 1024;  // 100MB
    std::unique_ptr<PersistenceManager> persistenceManager_;
    std::unique_ptr<ProcessingManager> processingManager_;
};

