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
    void setZeroCopyMode(bool enable) { zeroCopyMode_ = enable; }

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
    bool zeroCopyMode_;
    
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
    bool sendMboMessage(int clientSocket, const databento::MboMsg& mbo);
    bool sendMboMessageFast(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp);
    bool sendData(int socket, const void* data, size_t size);
    bool sendFileZeroCopy(int clientSocket);
};
