#include "project/tcp_sender.hpp"
#include "project/utils.hpp"
#include <chrono>
#include <cstring>
#include <algorithm>
#include <databento/dbn_file_store.hpp>
#include <span>
 
TCPSender::TCPSender() 
    : port_(8080), batchSize_(100),
      serverSocket_(-1), clientSocket_(-1),
      streaming_(false), sentMessages_(0) {
}

TCPSender::~TCPSender() {
    stopStreaming();
}

bool TCPSender::setupServer() {
    // Create socket
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ == -1) {
        utils::logError("Failed to create socket");
        return false;
    }
    
    // Socket options
    int flag = 1;
    if (setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set TCP_NODELAY");
    }
    
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set SO_REUSEADDR");
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
    
    // Listen
    if (listen(serverSocket_, 1) == -1) {
        utils::logError("Failed to listen on socket");
        close(serverSocket_);
        serverSocket_ = -1;
        return false;
    }
    
    // Ready
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
    // Wait for client
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    
    clientSocket_ = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientAddrLen);
    if (clientSocket_ == -1) {
        utils::logError("Failed to accept client connection");
        return;
    }
    
    // Client socket options
    int flag = 1;
    if (setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set TCP_NODELAY on client socket");
    }
    // Larger send buffer for throughput
    int sendBufSize = 32 * 1024 * 1024; // 32 MiB
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        utils::logWarning("Failed to set SO_SNDBUF on client socket");
    }
    
    // Connected
    
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
    
    // Load and pre-parse the entire DBN file
    std::unique_ptr<databento::DbnFileStore> store;
    try {
        store = std::make_unique<databento::DbnFileStore>(dataFile_);
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
    
    // Batch I/O with writev
    std::vector<MboMessage> batchBuffer;
    batchBuffer.reserve(batchSize_);
    // Start streaming messages - timing starts here
    startTime_ = std::chrono::steady_clock::now();
    
    try {
        for (size_t i = 0; i < allMessages.size() && streaming_; ++i) {
            if (stopToken.stop_requested()) {
                break;
            }
            const auto& mbo = allMessages[i];
            
            // Preserve original timestamp to maintain file order
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
            
            // No inter-message delay
        }
        // Send any remaining messages in the buffer
        if (!batchBuffer.empty() && sendBatchMessages(clientSocket_, batchBuffer)) {
            batchBuffer.clear();
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
    }
    
    auto streamEnd = std::chrono::steady_clock::now();
    endTime_ = streamEnd;
    
    // Close client socket to signal end
    close(clientSocket_);
    clientSocket_ = -1;
    streaming_ = false;
}

double TCPSender::getThroughput() const {
    if (sentMessages_ == 0) return 0.0;
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTimeToUse - startTime_);
    if (duration.count() == 0) return 0.0;
    
    return (double)sentMessages_ * 1000000.0 / duration.count();
}

long long TCPSender::getStreamingMs() const {
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTimeToUse - startTime_);
    return duration.count() / 1000; // microseconds to milliseconds
}

long long TCPSender::getStreamingUs() const {
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTimeToUse - startTime_);
    return duration.count();
}

bool TCPSender::sendBatchMessages(int clientSocket, const std::vector<MboMessage>& messages) {
    if (messages.empty()) {
        return true;
    }
    std::vector<struct iovec> iovecs;
    iovecs.reserve(messages.size());
    for (const auto& msg : messages) {
        struct iovec iov;
        std::span<const std::byte> msgSpan{reinterpret_cast<const std::byte*>(&msg), sizeof(MboMessage)};
        iov.iov_base = const_cast<void*>(reinterpret_cast<const void*>(msgSpan.data()));
        iov.iov_len = msgSpan.size();
        iovecs.push_back(iov);
    }
    ssize_t totalBytes = messages.size() * sizeof(MboMessage);
    std::span<const struct iovec> iovecsSpan{iovecs};
    ssize_t bytesSent = writev(clientSocket, const_cast<struct iovec*>(iovecsSpan.data()), static_cast<int>(iovecsSpan.size()));
    return bytesSent == totalBytes;
}

