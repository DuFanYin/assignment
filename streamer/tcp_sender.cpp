#include "tcp_sender.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>
#include <sys/uio.h>
#include <vector>
#ifdef __APPLE__
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <mach/mach_init.h>
#endif

TCPSender::TCPSender() 
    : host_("127.0.0.1"), port_(8080), delayMs_(0), zeroCopyMode_(false),
      streaming_(false), sentOrders_(0), connectedClients_(0), 
      fileDescriptor_(-1), mappedFile_(nullptr), fileSize_(0), serverSocket_(-1) {
}

TCPSender::~TCPSender() {
    stopStreaming();
    if (serverSocket_ != -1) {
        close(serverSocket_);
    }
    if (mappedFile_ != nullptr) {
        munmap(mappedFile_, fileSize_);
    }
    if (fileDescriptor_ != -1) {
        close(fileDescriptor_);
    }
}

bool TCPSender::loadFromFile(const std::string& filePath) {
    filePath_ = filePath;
    
    // Open file for memory mapping
    fileDescriptor_ = open(filePath.c_str(), O_RDONLY);
    if (fileDescriptor_ == -1) {
        std::cerr << "❌ Failed to open file: " << filePath << std::endl;
        return false;
    }
    
    // Get file size
    struct stat fileStat;
    if (fstat(fileDescriptor_, &fileStat) == -1) {
        std::cerr << "❌ Failed to get file size" << std::endl;
        close(fileDescriptor_);
        return false;
    }
    fileSize_ = fileStat.st_size;
    
    // Memory map the entire file
    mappedFile_ = mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
    if (mappedFile_ == MAP_FAILED) {
        std::cerr << "❌ Failed to memory map file" << std::endl;
        close(fileDescriptor_);
        return false;
    }
    
    // Also load with databento for parsing
    try {
        store_ = std::make_unique<databento::DbnFileStore>(filePath);
        auto meta = store_->GetMetadata();
        
        std::cout << "✅ Loaded DBN file: " << filePath << std::endl;
        std::cout << "✅ Schema: " << std::string(databento::ToString(meta.schema.value())) << std::endl;
        std::cout << "✅ Dataset: " << meta.dataset << std::endl;
        std::cout << "✅ File size: " << fileSize_ << " bytes (memory-mapped)" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to open DBN file: " << filePath << " - " << e.what() << std::endl;
        munmap(mappedFile_, fileSize_);
        close(fileDescriptor_);
        return false;
    }
}

bool TCPSender::startStreaming() {
    if (streaming_) {
        std::cout << "⚠️  Streaming already in progress" << std::endl;
        return false;
    }
    
    if (!store_) {
        std::cerr << "❌ No file loaded" << std::endl;
        return false;
    }
    
    if (!setupServer()) {
        return false;
    }
    
    streaming_ = true;
    streamingThread_ = std::thread(&TCPSender::streamingLoop, this);
    
    std::cout << "🚀 Started TCP streaming on " << host_ << ":" << port_ << std::endl;
    return true;
}

void TCPSender::stopStreaming() {
    if (streaming_) {
        streaming_ = false;
        if (streamingThread_.joinable()) {
            streamingThread_.join();
        }
        std::cout << "🛑 Stopped TCP streaming" << std::endl;
    }
}

bool TCPSender::setupServer() {
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ == -1) {
        std::cerr << "❌ Failed to create socket" << std::endl;
        return false;
    }
    
    int opt = 1;
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "❌ Failed to set socket options" << std::endl;
        close(serverSocket_);
        return false;
    }
    
    // Performance optimizations
    int nodelay = 1;
    if (setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1) {
        std::cerr << "⚠️  Failed to set TCP_NODELAY" << std::endl;
    }
    
    // Increase socket buffer sizes for maximum throughput
    int sendBufSize = 16 * 1024 * 1024; // 16MB send buffer
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_SNDBUF" << std::endl;
    }
    
    int recvBufSize = 16 * 1024 * 1024; // 16MB receive buffer
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_RCVBUF" << std::endl;
    }
    
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(host_.c_str());
    serverAddr.sin_port = htons(port_);
    
    if (bind(serverSocket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "❌ Failed to bind socket to " << host_ << ":" << port_ << std::endl;
        close(serverSocket_);
        return false;
    }
    
    if (listen(serverSocket_, 5) == -1) {
        std::cerr << "❌ Failed to listen on socket" << std::endl;
        close(serverSocket_);
        return false;
    }
    
    std::cout << "✅ TCP server listening on " << host_ << ":" << port_ << std::endl;
    return true;
}

void TCPSender::streamingLoop() {
    // Set CPU affinity to CPU 0 for maximum performance (macOS compatible)
#ifdef __APPLE__
    thread_affinity_policy_data_t affinityPolicy;
    affinityPolicy.affinity_tag = 0; // CPU 0
    thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, 
                     (thread_policy_t)&affinityPolicy, THREAD_AFFINITY_POLICY_COUNT);
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "⚠️  Failed to set CPU affinity" << std::endl;
    }
#endif
    
    // Set thread to real-time priority
    struct sched_param param;
    param.sched_priority = 99; // Maximum priority
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "⚠️  Failed to set real-time priority" << std::endl;
    }
    
    std::cout << "📡 Waiting for client connection..." << std::endl;
    
    // Wait for single client connection
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    
    int clientSocket = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientLen);
    if (clientSocket == -1) {
        std::cerr << "❌ Failed to accept client connection" << std::endl;
        streaming_ = false;
        return;
    }
    
    // Set performance options for client socket
    int nodelay = 1;
    if (setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1) {
        std::cerr << "⚠️  Failed to set TCP_NODELAY on client socket" << std::endl;
    }
    
    // Increase socket buffer sizes for maximum throughput
    int sendBufSize = 16 * 1024 * 1024; // 16MB send buffer
    if (setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_SNDBUF on client socket" << std::endl;
    }
    
    int recvBufSize = 16 * 1024 * 1024; // 16MB receive buffer
    if (setsockopt(clientSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize)) == -1) {
        std::cerr << "⚠️  Failed to set SO_RCVBUF on client socket" << std::endl;
    }
    
    connectedClients_ = 1;
    std::cout << "🔗 Client connected from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
    
    // Wait for verification/start signal from client
    std::cout << "⏳ Waiting for start signal from client..." << std::endl;
    char buffer[1024];
    ssize_t bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytesReceived <= 0) {
        std::cerr << "❌ Failed to receive start signal from client" << std::endl;
        close(clientSocket);
        streaming_ = false;
        return;
    }
    
    buffer[bytesReceived] = '\0';
    std::string startSignal(buffer);
    
    if (startSignal.find("START_STREAMING") != std::string::npos) {
        std::cout << "🚀 Received start signal! Beginning high-performance streaming..." << std::endl;
    } else {
        std::cout << "⚠️  Received unknown signal: " << startSignal << std::endl;
        std::cout << "🚀 Starting streaming anyway..." << std::endl;
    }
    
    // Choose streaming mode based on configuration
    if (zeroCopyMode_) {
        std::cout << "🚀 Using ZERO-COPY mode (sendfile) for maximum performance..." << std::endl;
        if (sendFileZeroCopy(clientSocket)) {
            sentOrders_ = fileSize_ / 30; // Approximate message count
            std::cout << "✅ Zero-copy streaming completed!" << std::endl;
        } else {
            std::cerr << "❌ Zero-copy streaming failed" << std::endl;
        }
    } else {
        std::cout << "🚀 Using ULTRA-FAST PRE-PARSED mode..." << std::endl;
        std::cout << "🚀 Pre-parsing entire file into memory for maximum speed..." << std::endl;
        
        // Pre-parse entire file into memory for maximum performance
        std::vector<databento::MboMsg> allMessages;
        allMessages.reserve(1000000); // Reserve space for ~1M messages
        
        std::cout << "📊 Pre-parsing file into memory..." << std::endl;
        auto parseStart = std::chrono::high_resolution_clock::now();
        
        // Reset file to beginning
        store_ = std::make_unique<databento::DbnFileStore>(filePath_);
        
        while (true) {
            const auto* record = store_->NextRecord();
            if (!record) break;
            
            if (record->RType() == databento::RType::Mbo) {
                allMessages.push_back(record->Get<databento::MboMsg>());
            }
        }
        
        auto parseEnd = std::chrono::high_resolution_clock::now();
        auto parseTime = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart);
        std::cout << "✅ Pre-parsed " << allMessages.size() << " messages in " << parseTime.count() << "ms" << std::endl;
        
        // Get timestamp once for all messages (or use file timestamps)
        auto baseTimestamp = std::chrono::high_resolution_clock::now();
        auto baseTime = std::chrono::duration_cast<std::chrono::microseconds>(baseTimestamp.time_since_epoch()).count();
        
        // Ultra-fast streaming loop - individual messages, no batching
        std::cout << "🚀 Starting ultra-fast streaming (individual messages)..." << std::endl;
        auto streamStart = std::chrono::high_resolution_clock::now();
        
        try {
            for (size_t i = 0; i < allMessages.size() && streaming_; ++i) {
                const auto& mbo = allMessages[i];
                
                // Send individual message with minimal overhead
                if (!sendMboMessageFast(clientSocket, mbo, baseTime + i)) {
                    std::cout << "🔌 Client disconnected during streaming" << std::endl;
                    break;
                }
                
                sentOrders_++;
            }
            
            auto streamEnd = std::chrono::high_resolution_clock::now();
            auto streamTime = std::chrono::duration_cast<std::chrono::milliseconds>(streamEnd - streamStart);
            auto messagesPerSecond = (allMessages.size() * 1000.0) / streamTime.count();
            
            std::cout << "✅ Ultra-fast streaming completed!" << std::endl;
            std::cout << "📊 Performance: " << messagesPerSecond << " messages/sec" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "❌ Error during streaming: " << e.what() << std::endl;
        }
    }
    
    close(clientSocket);
    connectedClients_ = 0;
    streaming_ = false;
    std::cout << "✅ Streaming completed" << std::endl;
}


bool TCPSender::sendMboMessage(int clientSocket, const databento::MboMsg& mbo) {
    // Get current timestamp for latency measurement
    auto now = std::chrono::high_resolution_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    
    // Optimized binary message format for maximum performance
    // Format: [timestamp:8][order_id:8][price:8][size:4][action:1][side:1]
    struct MboMessage {
        uint64_t timestamp;
        uint64_t order_id;
        uint64_t price;
        uint32_t size;
        uint8_t action;
        uint8_t side;
    } __attribute__((packed));
    
    MboMessage msg;
    msg.timestamp = timestamp;
    msg.order_id = mbo.order_id;
    msg.price = mbo.price;
    msg.size = mbo.size;
    msg.action = static_cast<uint8_t>(mbo.action);
    msg.side = static_cast<uint8_t>(mbo.side);
    
    return sendData(clientSocket, &msg, sizeof(msg));
}

bool TCPSender::sendMboMessageFast(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp) {
    // Ultra-fast version - no timestamp calculation overhead
    // Format: [timestamp:8][order_id:8][price:8][size:4][action:1][side:1]
    struct MboMessage {
        uint64_t timestamp;
        uint64_t order_id;
        uint64_t price;
        uint32_t size;
        uint8_t action;
        uint8_t side;
    } __attribute__((packed));
    
    MboMessage msg;
    msg.timestamp = timestamp;
    msg.order_id = mbo.order_id;
    msg.price = mbo.price;
    msg.size = mbo.size;
    msg.action = static_cast<uint8_t>(mbo.action);
    msg.side = static_cast<uint8_t>(mbo.side);
    
    return sendData(clientSocket, &msg, sizeof(msg));
}

bool TCPSender::sendData(int socket, const void* data, size_t size) {
    ssize_t bytesSent = write(socket, data, size);
    return bytesSent == static_cast<ssize_t>(size);
}

bool TCPSender::sendFileZeroCopy(int clientSocket) {
    // Use sendfile() for zero-copy file-to-socket transfer (macOS compatible)
#ifdef __APPLE__
    off_t offset = 0;
    off_t bytesSent = 0;
    int result = sendfile(fileDescriptor_, clientSocket, offset, &bytesSent, nullptr, 0);
    
    if (result == -1) {
        std::cerr << "❌ sendfile() failed: " << strerror(errno) << std::endl;
        return false;
    }
#else
    off_t offset = 0;
    ssize_t bytesSent = sendfile(clientSocket, fileDescriptor_, &offset, fileSize_);
    
    if (bytesSent == -1) {
        std::cerr << "❌ sendfile() failed: " << strerror(errno) << std::endl;
        return false;
    }
#endif
    
    std::cout << "✅ Zero-copy transfer completed: " << bytesSent << " bytes" << std::endl;
    return true;
}
