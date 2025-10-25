#include "tcp_sender.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <databento/dbn_file_store.hpp>

#ifdef __APPLE__
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <mach/mach_init.h>
#endif

TCPSender::TCPSender() 
    : port_(8080), delayMs_(0), zeroCopyMode_(false), 
      serverSocket_(-1), clientSocket_(-1), fileDescriptor_(-1),
      mappedFile_(nullptr), fileSize_(0), streaming_(false), sentMessages_(0) {
}

TCPSender::~TCPSender() {
    stopStreaming();
    cleanup();
}

bool TCPSender::loadFromFile(const std::string& filePath) {
    // Open file for memory mapping
    fileDescriptor_ = open(filePath.c_str(), O_RDONLY);
    if (fileDescriptor_ == -1) {
        utils::logError("Failed to open file: " + filePath);
        return false;
    }
    
    // Get file size
    struct stat fileStat;
    if (fstat(fileDescriptor_, &fileStat) == -1) {
        utils::logError("Failed to get file size");
        close(fileDescriptor_);
        fileDescriptor_ = -1;
        return false;
    }
    fileSize_ = fileStat.st_size;
    
    // Memory map the file
    mappedFile_ = mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
    if (mappedFile_ == MAP_FAILED) {
        utils::logError("Failed to memory map file");
        close(fileDescriptor_);
        fileDescriptor_ = -1;
        return false;
    }
    
    utils::logInfo("Loaded file: " + filePath + " (" + std::to_string(fileSize_) + " bytes)");
    return true;
}

bool TCPSender::setupServer() {
    // Create socket
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ == -1) {
        utils::logError("Failed to create socket");
        return false;
    }
    
    // Set socket options for high performance
    int flag = 1;
    if (setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set TCP_NODELAY");
    }
    
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set SO_REUSEADDR");
    }
    
    // Set large send buffer (16MB)
    int sendBufSize = 16 * 1024 * 1024;
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        utils::logWarning("Failed to set SO_SNDBUF");
    }
    
    // Configure server address
    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_addr.s_addr = INADDR_ANY;
    serverAddr_.sin_port = htons(port_);
    
    // Bind socket
    if (bind(serverSocket_, (struct sockaddr*)&serverAddr_, sizeof(serverAddr_)) == -1) {
        utils::logError("Failed to bind socket to port " + std::to_string(port_));
        close(serverSocket_);
        serverSocket_ = -1;
        return false;
    }
    
    // Listen for connections
    if (listen(serverSocket_, 1) == -1) {
        utils::logError("Failed to listen on socket");
        close(serverSocket_);
        serverSocket_ = -1;
        return false;
    }
    
    utils::logInfo("TCP server listening on port " + std::to_string(port_));
    return true;
}

void TCPSender::startStreaming() {
    if (streaming_) {
        utils::logWarning("Streaming already in progress");
        return;
    }
    
    if (!setupServer()) {
        return;
    }
    
    streaming_ = true;
    streamingThread_ = std::thread(&TCPSender::streamingLoop, this);
}

void TCPSender::stopStreaming() {
    if (!streaming_) {
        return;
    }
    
    streaming_ = false;
    
    if (streamingThread_.joinable()) {
        streamingThread_.join();
    }
    
    if (clientSocket_ != -1) {
        close(clientSocket_);
        clientSocket_ = -1;
    }
    
    if (serverSocket_ != -1) {
        close(serverSocket_);
        serverSocket_ = -1;
    }
}

void TCPSender::streamingLoop() {
    // Wait for client connection
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    
    utils::logInfo("Waiting for client connection...");
    clientSocket_ = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSocket_ == -1) {
        utils::logError("Failed to accept client connection");
        return;
    }
    
    // Set client socket options for high performance
    int flag = 1;
    if (setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set TCP_NODELAY on client socket");
    }
    
    int sendBufSize = 16 * 1024 * 1024;
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        utils::logWarning("Failed to set SO_SNDBUF on client socket");
    }
    
    utils::logInfo("Client connected from " + std::string(inet_ntoa(clientAddr.sin_addr)));
    
    // Wait for START_STREAMING signal
    char buffer[32];
    ssize_t bytesReceived = recv(clientSocket_, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        utils::logError("Failed to receive START_STREAMING signal");
        return;
    }
    
    buffer[bytesReceived] = '\0';
    if (strcmp(buffer, "START_STREAMING") != 0) {
        utils::logError("Invalid signal received: " + std::string(buffer));
        return;
    }
    
    utils::logInfo("Received START_STREAMING signal, beginning data transmission...");
    
    // Set CPU affinity and real-time priority for maximum performance
#ifdef __APPLE__
    // macOS CPU affinity
    thread_affinity_policy_data_t affinityPolicy;
    affinityPolicy.affinity_tag = 0; // CPU 0
    kern_return_t result = thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, 
                                            (thread_policy_t)&affinityPolicy, THREAD_AFFINITY_POLICY_COUNT);
    if (result != KERN_SUCCESS) {
        utils::logWarning("Failed to set CPU affinity");
    }
    
    // macOS real-time priority
    struct sched_param param;
    param.sched_priority = 99;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        utils::logWarning("Failed to set real-time priority");
    }
#endif
    
    // Load and pre-parse the entire DBN file for maximum performance
    utils::logInfo("Pre-parsing DBN file for ultra-fast streaming...");
    std::unique_ptr<databento::DbnFileStore> store;
    try {
        store = std::make_unique<databento::DbnFileStore>("/Users/hang/github_repo/assignment/src/data/CLX5_mbo.dbn");
    } catch (const std::exception& e) {
        utils::logError("Failed to open DBN file: " + std::string(e.what()));
        return;
    }
    
    // Pre-parse all MBO messages into memory
    std::vector<databento::MboMsg> allMessages;
    allMessages.reserve(1000000); // Reserve space for 1M messages
    
    const databento::Record* record;
    while ((record = store->NextRecord()) != nullptr) {
        if (record->RType() == databento::RType::Mbo) {
            allMessages.push_back(record->Get<databento::MboMsg>());
        }
    }
    
    utils::logInfo("Pre-parsed " + std::to_string(allMessages.size()) + " MBO messages");
    
    // Get timestamp once for all messages (or use file timestamps)
    auto baseTimestamp = std::chrono::high_resolution_clock::now();
    auto baseTime = std::chrono::duration_cast<std::chrono::microseconds>(baseTimestamp.time_since_epoch()).count();
    
    // Ultra-fast streaming loop - individual messages, no batching
    std::cout << "🚀 Starting ultra-fast streaming (individual messages)..." << std::endl;
    auto streamStart = std::chrono::high_resolution_clock::now();
    
    try {
        for (size_t i = 0; i < allMessages.size() && streaming_; ++i) {
            const auto& mbo = allMessages[i];
            
            // Send message with calculated timestamp
            if (!sendMboMessageFast(clientSocket_, mbo, baseTime + i)) {
                utils::logError("Failed to send message " + std::to_string(i));
                break;
            }
            
            sentMessages_++;
            
            // Optional delay (set to 0 for maximum throughput)
            if (delayMs_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
            }
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
    }
    
    auto streamEnd = std::chrono::high_resolution_clock::now();
    auto streamDuration = std::chrono::duration_cast<std::chrono::milliseconds>(streamEnd - streamStart);
    
    // Final statistics
    std::cout << "\n=== TCP Sender Final Statistics ===" << std::endl;
    std::cout << "Streaming Time: " << streamDuration.count() << " ms" << std::endl;
    std::cout << "Messages Sent: " << sentMessages_ << std::endl;
    if (streamDuration.count() > 0) {
        double messagesPerSecond = (double)sentMessages_ * 1000.0 / streamDuration.count();
        std::cout << "Throughput: " << std::fixed << std::setprecision(0) << messagesPerSecond << " messages/sec" << std::endl;
    }
    std::cout << "===================================" << std::endl;
    
    utils::logInfo("Data transmission completed successfully!");
    
    // Close client socket to signal end of transmission
    close(clientSocket_);
    clientSocket_ = -1;
}

bool TCPSender::sendMboMessage(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp) {
    MboMessage msg;
    msg.ts_event = timestamp;
    msg.ts_recv = timestamp + 1;
    msg.rtype = static_cast<uint8_t>(mbo.hd.rtype);
    msg.publisher_id = mbo.hd.publisher_id;
    msg.instrument_id = mbo.hd.instrument_id;
    msg.action = static_cast<uint8_t>(mbo.action);
    msg.side = static_cast<uint8_t>(mbo.side);
    msg.price = mbo.price;
    msg.size = mbo.size;
    msg.channel_id = mbo.channel_id;
    msg.order_id = mbo.order_id;
    msg.flags = static_cast<uint8_t>(mbo.flags);
    msg.ts_in_delta = mbo.ts_in_delta.count();
    msg.sequence = mbo.sequence;
    
    return sendData(clientSocket, &msg, sizeof(msg));
}

bool TCPSender::sendMboMessageFast(int clientSocket, const databento::MboMsg& mbo, uint64_t timestamp) {
    // Send FULL message with all 14 fields - exactly like the working streamer approach
    MboMessage msg;
    msg.ts_event = timestamp;
    msg.ts_recv = timestamp + 1;
    msg.rtype = static_cast<uint8_t>(mbo.hd.rtype);
    msg.publisher_id = mbo.hd.publisher_id;
    msg.instrument_id = mbo.hd.instrument_id;
    msg.action = static_cast<uint8_t>(mbo.action);
    msg.side = static_cast<uint8_t>(mbo.side);
    msg.price = mbo.price;
    msg.size = mbo.size;
    msg.channel_id = mbo.channel_id;
    msg.order_id = mbo.order_id;
    msg.flags = static_cast<uint8_t>(mbo.flags);
    msg.ts_in_delta = mbo.ts_in_delta.count();
    msg.sequence = mbo.sequence;
    
    // Send complete message directly - same approach as working streamer
    return sendData(clientSocket, &msg, sizeof(msg));
}

bool TCPSender::sendData(int clientSocket, const void* data, size_t size) {
    ssize_t bytesSent = write(clientSocket, data, size);
    return bytesSent == static_cast<ssize_t>(size);
}

bool TCPSender::sendFileZeroCopy(int clientSocket) {
    if (!mappedFile_ || fileSize_ == 0) {
        return false;
    }
    
#ifdef __APPLE__
    // macOS sendfile - correct signature
    off_t offset = 0;
    off_t bytesSent = 0;
    int result = sendfile(fileDescriptor_, clientSocket, offset, &bytesSent, nullptr, 0);
    return result == 0 && bytesSent == static_cast<off_t>(fileSize_);
#else
    // Linux sendfile
    ssize_t bytesSent = sendfile(clientSocket, fileDescriptor_, nullptr, fileSize_);
    return bytesSent == static_cast<ssize_t>(fileSize_);
#endif
}

double TCPSender::getThroughput() const {
    if (sentMessages_ == 0) return 0.0;
    
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_);
    
    if (duration.count() == 0) return 0.0;
    
    return (double)sentMessages_ * 1000.0 / duration.count();
}

void TCPSender::cleanup() {
    if (mappedFile_ != nullptr && mappedFile_ != MAP_FAILED) {
        munmap(mappedFile_, fileSize_);
        mappedFile_ = nullptr;
    }
    
    if (fileDescriptor_ != -1) {
        close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
}
