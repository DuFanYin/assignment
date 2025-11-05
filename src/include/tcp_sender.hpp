#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <databento/record.hpp>
#include <databento/enums.hpp>

class TCPSender {
public:
    TCPSender();
    ~TCPSender();

    // Configuration
    void setDelayMs(int delayMs) { delayMs_ = delayMs; }
    void setZeroCopyMode(bool enabled) { zeroCopyMode_ = enabled; }
    void setPort(int port) { port_ = port; }
    void setBatchSize(size_t batchSize) { batchSize_ = batchSize; }

    // Data loading and streaming
    bool loadFromFile(const std::string& filePath);
    void startStreaming();
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // Statistics
    size_t getSentMessages() const { return sentMessages_; }
    double getThroughput() const;

private:
    // Configuration
    int port_;
    int delayMs_;
    bool zeroCopyMode_;
    size_t batchSize_;
    
    // Network
    int serverSocket_;
    int clientSocket_;
    struct sockaddr_in serverAddr_;
    
    // File handling
    int fileDescriptor_;
    void* mappedFile_;
    size_t fileSize_;
    
    // State
    std::atomic<bool> streaming_;
    std::atomic<size_t> sentMessages_;
    std::thread streamingThread_;
    
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
    void streamingLoop();
    bool sendMboMessage(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp);
    bool sendMboMessageFast(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp);
    bool sendBatchMessages(int clientSocket, const std::vector<MboMessage>& messages);
    bool sendData(int clientSocket, const void* data, size_t size);
    bool sendFileZeroCopy(int clientSocket);
    void cleanup();
};
