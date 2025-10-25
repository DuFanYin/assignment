#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <databento/dbn_file_store.hpp>
#include <databento/record.hpp>
#include <databento/enums.hpp>

class TCPSender {
public:
    TCPSender();
    ~TCPSender();

    // Configuration
    void setPort(int port) { port_ = port; }
    void setHost(const std::string& host) { host_ = host; }
    void setDelayMs(int delayMs) { delayMs_ = delayMs; }
    void setBatchMode(bool enable) { batchMode_ = enable; }
    void setBatchSize(size_t batchSize) { batchSize_ = batchSize; }

    // Data streaming
    bool loadFromFile(const std::string& filePath);
    bool startStreaming();
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // Statistics
    size_t getSentOrders() const { return sentOrders_; }
    size_t getConnectedClients() const { return connectedClients_; }

private:
    // Configuration
    std::string host_;
    int port_;
    int delayMs_;
    bool batchMode_;
    size_t batchSize_;
    
    // State
    std::atomic<bool> streaming_;
    std::atomic<size_t> sentOrders_;
    std::atomic<size_t> connectedClients_;
    
    // Data source
    std::unique_ptr<databento::DbnFileStore> store_;
    std::string filePath_;
    int fileDescriptor_;
    void* mappedFile_;
    size_t fileSize_;
    
    // Networking
    int serverSocket_;
    std::thread streamingThread_;
    
    // Methods
    void streamingLoop();
    bool setupServer();
    bool sendData(int socket, const void* data, size_t size);
    bool sendBatchMessagesBytes(int clientSocket, const std::vector<std::vector<uint8_t>>& messages, uint64_t baseTimestamp);
};
