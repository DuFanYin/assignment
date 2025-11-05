#include "tcp_sender.hpp"
#include "utils.hpp"
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
    : port_(8080), batchSize_(100),
      serverSocket_(-1), clientSocket_(-1),
      streaming_(false), sentMessages_(0) {
}

TCPSender::~TCPSender() {
    stopStreaming();
}

// Removed unused file mapping path

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
    
    // Note: Socket buffer optimization removed to avoid warnings
    
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
    
    // TCP server listening on port
    return true;
}

void TCPSender::startStreaming() {
    if (streaming_) {
        utils::logWarning("Streaming already in progress");
        streaming_ = false;
        return;
    }
    
    if (!setupServer()) {
        streaming_ = false;
        return;
    }
    
    streaming_ = true;
    streamingThread_ = std::jthread([this](std::stop_token st){ this->streamingLoop(st); });
}

void TCPSender::stopStreaming() {
    if (!streaming_) {
        return;
    }
    
    streaming_ = false;
    if (streamingThread_.joinable()) {
        // Unblock accept/recv by shutting down sockets
        if (clientSocket_ != -1) {
            shutdown(clientSocket_, SHUT_RDWR);
        }
        if (serverSocket_ != -1) {
            shutdown(serverSocket_, SHUT_RDWR);
        }
        streamingThread_.request_stop();
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

void TCPSender::streamingLoop(std::stop_token stopToken) {
    // Wait for client connection
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    
    // Waiting for client connection
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
    // Increase sender send buffer for higher throughput
    int sendBufSize = 32 * 1024 * 1024; // 32 MiB
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        utils::logWarning("Failed to set SO_SNDBUF on client socket");
    }
    
    // Note: Client socket buffer optimization removed to avoid warnings
    
    // Client connected successfully
    
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
        streaming_ = false;
        return;
    }
    
    // Note: CPU affinity and real-time priority optimizations removed to avoid warnings
    
    // Load and pre-parse the entire DBN file for maximum performance
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
    
    // Pre-parsed MBO messages for streaming
    
    // Batch I/O with writev for maximum throughput
    std::vector<MboMessage> batchBuffer;
    batchBuffer.reserve(batchSize_);
    // Start streaming messages
    auto streamStart = std::chrono::high_resolution_clock::now();
    startTime_ = streamStart;
    
    try {
        for (size_t i = 0; i < allMessages.size() && streaming_; ++i) {
            if (stopToken.stop_requested()) {
                break;
            }
            const auto& mbo = allMessages[i];
            
            // Prepare message with ORIGINAL timestamp to preserve file order
            uint64_t originalTimestamp = mbo.hd.ts_event.time_since_epoch().count();
            MboMessage msg;
            msg.ts_event = originalTimestamp;
            msg.ts_recv = originalTimestamp + 1;
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
            
            batchBuffer.push_back(msg);
            sentMessages_++;
            
            if (batchBuffer.size() >= batchSize_) {
                if (!sendBatchMessages(clientSocket_, batchBuffer)) {
                    utils::logError("Failed to send batch at message " + std::to_string(i));
                    break;
                }
                batchBuffer.clear();
            }
            
            // No inter-message delay (maximum throughput)
        }
        // Send any remaining messages in the buffer
        if (!batchBuffer.empty() && sendBatchMessages(clientSocket_, batchBuffer)) {
            batchBuffer.clear();
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
    }
    
    auto streamEnd = std::chrono::high_resolution_clock::now();
    endTime_ = streamEnd;
    
    // Close client socket to signal end of transmission
    close(clientSocket_);
    clientSocket_ = -1;
    streaming_ = false;
}

bool TCPSender::sendBatchMessages(int clientSocket, const std::vector<MboMessage>& messages) {
    if (messages.empty()) {
        return true;
    }
    std::vector<struct iovec> iovecs;
    iovecs.reserve(messages.size());
    for (const auto& msg : messages) {
        struct iovec iov;
        iov.iov_base = reinterpret_cast<void*>(const_cast<MboMessage*>(&msg));
        iov.iov_len = sizeof(MboMessage);
        iovecs.push_back(iov);
    }
    ssize_t totalBytes = messages.size() * sizeof(MboMessage);
    ssize_t bytesSent = writev(clientSocket, iovecs.data(), static_cast<int>(iovecs.size()));
    return bytesSent == totalBytes;
}

double TCPSender::getThroughput() const {
    if (sentMessages_ == 0) return 0.0;
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - startTime_);
    if (duration.count() == 0) return 0.0;
    
    return (double)sentMessages_ * 1000.0 / duration.count();
}

long long TCPSender::getStreamingMs() const {
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - startTime_);
    return duration.count();
}

// Removed unused helpers
