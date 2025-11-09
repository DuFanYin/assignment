#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include <random>
#include "project/order_book.hpp"
#include "project/ring_buffer.hpp"
#include "project/book_snapshot.hpp"
#include <databento/constants.hpp>
#include <databento/record.hpp>

namespace db = databento;

// Message wrapper for ring buffer
struct MboMessageWrapper {
    BookSnapshot snapshot;
    std::chrono::steady_clock::time_point timestamp;
    
    MboMessageWrapper() = default;
    explicit MboMessageWrapper(const BookSnapshot& snap) 
        : snapshot(snap), timestamp(std::chrono::steady_clock::now()) {}
};

// Forward declarations
namespace project {
    class DatabaseWriter;
    class JSONGenerator;
}

// Include PostgresConnection for Config (no circular dependency since database_writer doesn't include websocket_server)
#include "database/postgres_connection.hpp"

class WebSocketServer {
public:
    WebSocketServer(int port, const PostgresConnection::Config& dbConfig,
                    size_t topLevels = 10,
                    bool outputFullBook = true,
                    size_t ringBufferSize = 65536);
    ~WebSocketServer();
    
    bool start();
    void stop();
    bool isRunning() const { return isServerRunning_; }
    
    // Statistics
    size_t getMessagesProcessed() const { return totalMessagesProcessed_; }
    size_t getBytesReceived() const { return totalBytesReceived_; }
    
    struct PerSocketData {
        std::vector<uint8_t> buffer;
        size_t totalBytesReceived = 0;
        bool isMetadataReceived = false;
        std::string fileName;
        size_t fileSize = 0;
        std::string tempFilePath;
        bool isProcessingStarted = false;
        std::function<void(const std::string&)> sendMessage; // Callback to send messages
    };
    
private:
    void processDbnChunk(const std::vector<uint8_t>& chunk, PerSocketData* socketData, 
                        const std::function<void(const std::string&)>& sendMessage = nullptr);
    void databaseWriterLoop(std::stop_token stopToken);
    
    int port_;
    PostgresConnection::Config databaseConfig_;
    std::atomic<bool> isServerRunning_;
    std::atomic<size_t> totalMessagesProcessed_;
    std::atomic<size_t> totalBytesReceived_;
    
    // Order book and database writing
    std::unique_ptr<Book> orderBook_;
    std::unique_ptr<RingBuffer<MboMessageWrapper>> snapshotRingBuffer_;  // Ring buffer for snapshots to be written to DB
    std::jthread databaseWriterThread_;  // Database writer thread (C++20 jthread)
    
    std::unique_ptr<project::DatabaseWriter> databaseWriter_;
    std::unique_ptr<project::JSONGenerator> jsonGenerator_;
    
    // Configuration
    std::string symbol_;
    std::string activeSessionId_;  // Cached session ID - avoid touching databaseWriter_ from hot path
    size_t topLevels_;
    bool outputFullBook_;
    
    // Processing thread timing
    std::chrono::steady_clock::time_point processingStartTime_;
    std::chrono::steady_clock::time_point processingEndTime_;
    
    // Processing thread statistics (same as receiver version)
    std::atomic<size_t> processingMessagesReceived_;
    std::atomic<size_t> processingOrdersProcessed_;
    uint64_t processingTotalTimeNs_;
    std::atomic<uint64_t> processingTimingSamples_;
    static constexpr size_t kTimingReservoirSize = 8192;
    std::vector<uint64_t> processingTimingReservoir_;
    std::minstd_rand processingRng_;
    
    // Statistics getters (same as receiver version)
    double getThroughput() const;
    double getAverageOrderProcessNs() const;
    uint64_t getP99OrderProcessNs() const;
    
    // Session stats captured atomically to pass to DB thread
    struct SessionStats {
        size_t messagesReceived = 0;
        size_t ordersProcessed = 0;
        double throughput = 0.0;
        int64_t avgProcessNs = 0;
        uint64_t p99ProcessNs = 0;
        size_t totalOrders = 0;
        size_t bidLevels = 0;
        size_t askLevels = 0;
        double bestBid = 0.0;
        double bestAsk = 0.0;
        double spread = 0.0;
        bool hasBookState = false;
    };
    SessionStats sessionStats_;
};

