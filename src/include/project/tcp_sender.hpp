#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/uio.h>
#include <databento/record.hpp>
#include <databento/enums.hpp>

class TCPSender {
public:
    TCPSender();
    ~TCPSender();

    // Configuration
    void setPort(int port) { port_ = port; }
    void setBatchSize(size_t batchSize) { batchSize_ = batchSize; }
    void setDataFile(const std::string& dataFile) { dataFile_ = dataFile; }

    // Data loading and streaming
    void startStreaming();
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // Statistics
    size_t getSentMessages() const { return sentMessages_; }
    double getThroughput() const;
    long long getStreamingMs() const;
    long long getStreamingUs() const; // microseconds for higher precision

private:
    // Configuration
    int port_;
    size_t batchSize_;
    std::string dataFile_;
    
    // Network
    int serverSocket_;
    int clientSocket_;
    struct sockaddr_in serverAddr_;
    
    // State
    std::atomic<bool> streaming_;
    std::atomic<size_t> sentMessages_;
    std::jthread streamingThread_;
    
    // Timing
    std::chrono::high_resolution_clock::time_point startTime_;
    std::chrono::high_resolution_clock::time_point endTime_;
    
    // Binary message format for TCP transmission
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
    
    // Methods
    bool setupServer();
    void streamingLoop(std::stop_token stopToken);
    bool sendBatchMessages(int clientSocket, const std::vector<MboMessage>& messages);
};
