#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

class TCPReceiver {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    TCPReceiver();
    ~TCPReceiver();

    // Configuration
    void setHost(const std::string& host) { host_ = host; }
    void setPort(int port) { port_ = port; }
    void setMessageCallback(MessageCallback callback) { messageCallback_ = callback; }

    // Connection and streaming
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_; }

    // Statistics
    size_t getReceivedMessages() const { return receivedMessages_; }
    double getAverageLatencyMs() const { return averageLatencyMs_; }

private:
    // Configuration
    std::string host_;
    int port_;
    MessageCallback messageCallback_;
    
    // State
    std::atomic<bool> connected_;
    std::atomic<size_t> receivedMessages_;
    std::atomic<double> averageLatencyMs_;
    
    // Networking
    int clientSocket_;
    std::thread receiveThread_;
    
    // Methods
    void receiveLoop();
    bool setupConnection();
    bool receiveData(void* buffer, size_t size);
};
