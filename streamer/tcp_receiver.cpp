#include "tcp_receiver.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>

TCPReceiver::TCPReceiver() 
    : host_("127.0.0.1"), port_(8080), connected_(false), 
      receivedMessages_(0), averageLatencyMs_(0.0), clientSocket_(-1) {
}

TCPReceiver::~TCPReceiver() {
    disconnect();
}

bool TCPReceiver::connect() {
    if (connected_) {
        std::cout << "⚠️  Already connected" << std::endl;
        return true;
    }
    
    if (!setupConnection()) {
        return false;
    }
    
    connected_ = true;
    receiveThread_ = std::thread(&TCPReceiver::receiveLoop, this);
    
    std::cout << "🔗 Connected to " << host_ << ":" << port_ << std::endl;
    return true;
}

void TCPReceiver::disconnect() {
    if (connected_) {
        connected_ = false;
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
        if (clientSocket_ != -1) {
            close(clientSocket_);
            clientSocket_ = -1;
        }
        std::cout << "🔌 Disconnected from server" << std::endl;
    }
}

bool TCPReceiver::setupConnection() {
    clientSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket_ == -1) {
        std::cerr << "❌ Failed to create socket" << std::endl;
        return false;
    }
    
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(host_.c_str());
    serverAddr.sin_port = htons(port_);
    
    if (::connect(clientSocket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "❌ Failed to connect to " << host_ << ":" << port_ << std::endl;
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    // Performance optimizations
    int nodelay = 1;
    if (setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1) {
        std::cerr << "⚠️  Failed to set TCP_NODELAY" << std::endl;
    }
    
    // Increase socket buffer sizes for maximum throughput
    int recvBufSize = 16 * 1024 * 1024; // 16MB receive buffer
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_RCVBUF" << std::endl;
    }
    
    int sendBufSize = 16 * 1024 * 1024; // 16MB send buffer
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_SNDBUF" << std::endl;
    }
    
    // Send start signal to trigger streaming
    std::cout << "📤 Sending start signal to server..." << std::endl;
    const char* startSignal = "START_STREAMING";
    ssize_t bytesSent = write(clientSocket_, startSignal, strlen(startSignal));
    if (bytesSent != static_cast<ssize_t>(strlen(startSignal))) {
        std::cerr << "❌ Failed to send start signal" << std::endl;
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    std::cout << "✅ Start signal sent successfully" << std::endl;
    return true;
}

void TCPReceiver::receiveLoop() {
    std::cout << "📡 Starting receive loop..." << std::endl;
    
    // Binary message format: [timestamp:8][order_id:8][price:8][size:4][action:1][side:1]
    struct MboMessage {
        uint64_t timestamp;
        uint64_t order_id;
        uint64_t price;
        uint32_t size;
        uint8_t action;
        uint8_t side;
    } __attribute__((packed));
    
    // Standard buffer for individual messages
    char buffer[4096];
    size_t bufferPos = 0;
    
    try {
        while (connected_) {
            ssize_t bytesReceived = read(clientSocket_, buffer + bufferPos, sizeof(buffer) - bufferPos);
            
            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    std::cout << "🔌 Server disconnected" << std::endl;
                } else {
                    std::cerr << "❌ Error receiving data" << std::endl;
                }
                break;
            }
            
            bufferPos += bytesReceived;
            
            // Process complete binary messages
            while (bufferPos >= sizeof(MboMessage)) {
                MboMessage* msg = reinterpret_cast<MboMessage*>(buffer);
                
                receivedMessages_++;
                
                // Call the message callback if set
                if (messageCallback_) {
                    // Convert binary message to string for callback compatibility
                    std::ostringstream oss;
                    oss << "MBO:" << msg->order_id << ":" << msg->price << ":" 
                        << msg->size << ":" << static_cast<char>(msg->action) << ":" 
                        << static_cast<char>(msg->side) << ":" << msg->timestamp;
                    messageCallback_(oss.str());
                }
                
                // Move remaining data to front of buffer
                memmove(buffer, buffer + sizeof(MboMessage), bufferPos - sizeof(MboMessage));
                bufferPos -= sizeof(MboMessage);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ Error in receive loop: " << e.what() << std::endl;
    }
    
    connected_ = false;
}

bool TCPReceiver::receiveData(void* buffer, size_t size) {
    ssize_t bytesReceived = read(clientSocket_, buffer, size);
    return bytesReceived == static_cast<ssize_t>(size);
}
