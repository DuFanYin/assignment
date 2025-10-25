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
#include <climits>
#include <cstdint>

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
    std::cout << "📡 Starting batch receive loop..." << std::endl;
    
    // Simple byte-based approach - no alignment issues
    std::vector<uint8_t> buffer;
    buffer.reserve(1024 * 1024); // 1MB buffer
    size_t bufferPos = 0;
    
    try {
        std::cout << "🔍 Starting receive loop..." << std::endl;
        while (connected_) {
            // Prevent buffer overflow
            if (bufferPos >= buffer.capacity()) {
                std::cerr << "❌ Buffer overflow detected, clearing buffer" << std::endl;
                bufferPos = 0;
            }
            
            std::cout << "📥 Reading data, bufferPos=" << bufferPos << std::endl;
            ssize_t bytesReceived = read(clientSocket_, buffer.data() + bufferPos, buffer.capacity() - bufferPos);
            
            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    std::cout << "🔌 Server disconnected" << std::endl;
                } else {
                    std::cerr << "❌ Error reading from socket: " << strerror(errno) << std::endl;
                }
                break;
            }
            
            std::cout << "📊 Received " << bytesReceived << " bytes, total bufferPos=" << bufferPos + bytesReceived << std::endl;
            bufferPos += bytesReceived;
            
            // Process batch messages
            while (bufferPos >= 16) { // Minimum header size
                // Read batch header
                uint32_t batchSize = 0;
                for (int i = 0; i < 4; ++i) {
                    batchSize |= (static_cast<uint32_t>(buffer[i]) << (i * 8));
                }
                
                uint64_t baseTimestamp = 0;
                for (int i = 0; i < 8; ++i) {
                    baseTimestamp |= (static_cast<uint64_t>(buffer[4 + i]) << (i * 8));
                }
                
                // Skip reserved field (4 bytes)
                
                // Calculate total batch size
                size_t messageSize = 22; // order_id(8) + price(8) + size(4) + action(1) + side(1)
                size_t totalBatchSize = 16 + (batchSize * messageSize);
                
                std::cout << "📊 Parsed batch header: batchSize=" << batchSize << ", baseTimestamp=" << baseTimestamp << std::endl;
                
                if (bufferPos >= totalBatchSize) {
                    // Process entire batch
                    std::cout << "🔄 Processing batch with " << batchSize << " messages" << std::endl;
                    
                    size_t offset = 16; // Skip header
                    for (uint32_t i = 0; i < batchSize; ++i) {
                        // Read message data
                        uint64_t order_id = 0;
                        for (int j = 0; j < 8; ++j) {
                            order_id |= (static_cast<uint64_t>(buffer[offset + j]) << (j * 8));
                        }
                        offset += 8;
                        
                        uint64_t price = 0;
                        for (int j = 0; j < 8; ++j) {
                            price |= (static_cast<uint64_t>(buffer[offset + j]) << (j * 8));
                        }
                        offset += 8;
                        
                        uint32_t size = 0;
                        for (int j = 0; j < 4; ++j) {
                            size |= (static_cast<uint32_t>(buffer[offset + j]) << (j * 8));
                        }
                        offset += 4;
                        
                        uint8_t action = buffer[offset++];
                        uint8_t side = buffer[offset++];
                        
                        receivedMessages_++;
                        
                        // Call the message callback if set
                        if (messageCallback_) {
                            std::ostringstream oss;
                            oss << "MBO:" << order_id << ":" << price << ":" 
                                << size << ":" << static_cast<char>(action) << ":" 
                                << static_cast<char>(side) << ":" << (baseTimestamp + i);
                            messageCallback_(oss.str());
                        }
                    }
                    
                    // Move remaining data to front of buffer
                    size_t remainingBytes = bufferPos - totalBatchSize;
                    std::cout << "🔄 Moving " << remainingBytes << " remaining bytes to buffer start" << std::endl;
                    if (remainingBytes > 0) {
                        std::memmove(buffer.data(), buffer.data() + totalBatchSize, remainingBytes);
                    }
                    bufferPos = remainingBytes;
                    
                    std::cout << "📦 Processed batch of " << batchSize << " messages" << std::endl;
                } else {
                    // Need more data for complete batch
                    break;
                }
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
