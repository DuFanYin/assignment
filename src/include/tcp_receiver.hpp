#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <databento/record.hpp>
#include <databento/enums.hpp>
#include <databento/constants.hpp>
#include "order_book.hpp"

namespace db = databento;

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
    void enableJsonOutput(bool enable) { jsonOutputEnabled_ = enable; }
    void setJsonOutputFile(const std::string& filename) { jsonOutputFile_ = filename; }

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

private:
    // Configuration
    std::string host_;
    int port_;
    std::shared_ptr<Book> orderBook_;
    std::string symbol_;
    size_t topLevels_;
    bool outputFullBook_;
    bool jsonOutputEnabled_;
    std::string jsonOutputFile_;
    
    // Network
    int clientSocket_;
    struct sockaddr_in serverAddr_;
    
    // State
    std::atomic<bool> connected_;
    std::atomic<bool> receiving_;
    std::atomic<size_t> receivedMessages_;
    std::atomic<size_t> processedOrders_;
    std::atomic<size_t> jsonOutputs_;
    std::thread receivingThread_;
    
    // Timing
    std::chrono::high_resolution_clock::time_point startTime_;
    std::chrono::high_resolution_clock::time_point endTime_;
    
    // Binary message format matching sender
    struct MboMessage {
        uint64_t ts_event;
        uint64_t ts_recv;
        uint8_t rtype;
        uint16_t publisher_id;
        uint32_t instrument_id;
        uint8_t action;
        uint8_t side;
        int64_t price;
        uint32_t size;
        uint8_t channel_id;
        uint64_t order_id;
        uint8_t flags;
        int32_t ts_in_delta;
        uint32_t sequence;
    } __attribute__((packed));
    
    // Simple message format for high-performance streaming
    struct SimpleMboMessage {
        uint64_t timestamp;
        uint64_t order_id;
        uint64_t price;
        uint32_t size;
        uint8_t action;
        uint8_t side;
    } __attribute__((packed));
    
    // Methods
    bool setupConnection();
    void receivingLoop();
    bool receiveData(void* data, size_t size);
    databento::MboMsg convertSimpleToMboMsg(const SimpleMboMessage& msg);
    databento::MboMsg convertToMboMsg(const MboMessage& msg);
    void cleanup();
};
