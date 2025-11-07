#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <databento/record.hpp>
#include <databento/enums.hpp>
#include <databento/constants.hpp>
#include "order_book.hpp"
#include "message_types.hpp"
#include "ring_buffer.hpp"
#include <databento/datetime.hpp>
#include <sstream>
#include "json_writer.hpp"

namespace db = databento;

// Minimal snapshot types captured under writer lock
struct LevelEntry {
    int64_t price{db::kUndefPrice};
    uint32_t size{0};
    uint32_t count{0};
};

struct BookSnapshot {
    std::string_view symbol;
    int64_t ts_ns{0};
    PriceLevel bid{};
    PriceLevel ask{};
    std::vector<LevelEntry> bids; // top N bid levels, highest first
    std::vector<LevelEntry> asks; // top N ask levels, lowest first
    size_t total_orders{0};
    size_t bid_levels{0};
    size_t ask_levels{0};
};

// Message wrapper for ring buffer
struct MboMessageWrapper {
    BookSnapshot snapshot;
    std::chrono::steady_clock::time_point timestamp;
    
    MboMessageWrapper() = default;
    explicit MboMessageWrapper(const BookSnapshot& snap) : snapshot(snap) {
        timestamp = std::chrono::steady_clock::now();
    }
};

class TCPReceiver {
public:
    TCPReceiver();
    ~TCPReceiver();

    // Configuration
    void setHost(const std::string& host) { host_ = host; }
    void setPort(int port) { port_ = port; }
    void setOrderBook(std::shared_ptr<Book> orderBook) { orderBook_ = orderBook; }
    void setSymbol(const std::string& symbol) { symbol_ = symbol; }
    void setTopLevels(size_t levels) { topLevels_ = levels; }
    void setOutputFullBook(bool output) { outputFullBook_ = output; }
    void setJsonOutputFile(const std::string& filename) { jsonOutputFile_ = filename; }
    
    // JSON batching configuration
    void setJsonBatchSize(size_t size) { jsonBatchSize_ = size; }
    void setJsonFlushInterval(size_t interval) { jsonFlushInterval_ = interval; }

    // Connection and processing
    bool connect();
    void startReceiving();
    void stopReceiving();
    bool isConnected() const { return connected_; }

    // Statistics
    size_t getReceivedMessages() const { return receivedMessages_; }
    size_t getProcessedOrders() const { return processedOrders_; }
    double getThroughput() const;
    size_t getJsonOutputs() const { return jsonOutputs_; }
  double getAverageOrderProcessNs() const;
  uint64_t getP99OrderProcessNs() const;

private:
    // Configuration
    std::string host_;
    int port_;
    std::shared_ptr<Book> orderBook_;
    std::string symbol_;
    size_t topLevels_;
    bool outputFullBook_;
    // JSON output always enabled
    std::string jsonOutputFile_;
    
    // JSON batching
    size_t jsonBatchSize_;
    size_t jsonFlushInterval_;
    std::vector<std::string> jsonBuffer_;
    std::mutex jsonBufferMutex_;
    std::unique_ptr<MmapJsonWriter> jsonWriter_;
    
    // Network
    int clientSocket_;
    struct sockaddr_in serverAddr_;
    
    // State
    std::atomic<bool> connected_;
    std::atomic<bool> receiving_;
    std::atomic<size_t> receivedMessages_;
    std::atomic<size_t> processedOrders_;
    std::atomic<size_t> jsonOutputs_;
    std::jthread receivingThread_;
    std::jthread jsonGenerationThread_;  // Separate thread for JSON generation
    
    // Ring buffer for decoupling order book processing from JSON generation
    std::unique_ptr<RingBuffer<MboMessageWrapper>> jsonRingBuffer_;
    
    // Timing
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point endTime_;
    std::vector<uint64_t> orderProcessTimesNs_;  // Per-order processing times
    
    // Methods
    bool setupConnection();
    void receivingLoop(std::stop_token stopToken);
    void jsonGenerationLoop(std::stop_token stopToken);  // Runs in separate thread to generate JSON
    std::string generateJsonOutput(const BookSnapshot& snap);  // Generate JSON from captured snapshot
    bool receiveData(void* data, size_t size);
    databento::MboMsg convertToDatabentoMbo(const MboMessage& msg);
    void cleanup();
    
    // JSON batching methods
    void addJsonToBuffer(const std::string& json);
    void flushJsonBuffer();
    void flushJsonBufferInternal(); // Internal flush (assumes mutex is locked)
};
