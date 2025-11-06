#include "project/tcp_receiver.hpp"
#include "project/utils.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <fstream>
#include <databento/pretty.hpp>
#include <databento/constants.hpp>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <format>
#include <ranges>
#include <span>

TCPReceiver::TCPReceiver() 
    : host_("127.0.0.1"), port_(8080), orderBook_(nullptr), symbol_(""), 
      topLevels_(10), outputFullBook_(true),
      jsonBatchSize_(1000), jsonFlushInterval_(100), // Batch 1000 JSONs, flush every 100
      clientSocket_(-1), connected_(false), receiving_(false), 
      receivedMessages_(0), processedOrders_(0), jsonOutputs_(0),
      jsonRingBuffer_(std::make_unique<RingBuffer<MboMessageWrapper>>(65536)) {  // 64K ring buffer
    // Initialize JSON buffer
    jsonBuffer_.reserve(jsonBatchSize_);
    // Pre-reserve per-order timing storage to reduce reallocations
    orderProcessTimesNs_.reserve(100000);
}

TCPReceiver::~TCPReceiver() {
    // Only cleanup if not already stopped
    if (receiving_) {
        stopReceiving();
    }
    cleanup();
}

bool TCPReceiver::setupConnection() {
    // Create socket
    clientSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket_ == -1) {
        utils::logError("Failed to create socket");
        return false;
    }
    
    // Set socket options for high performance
    int flag = 1;
    if (setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
        utils::logWarning("Failed to set TCP_NODELAY");
    }
    
    // Set large receive buffer (16MB)
    int recvBufSize = 16 * 1024 * 1024;
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize)) == -1) {
        utils::logWarning("Failed to set SO_RCVBUF");
    }
    
    // Set large send buffer (16MB) for ACKs
    int sendBufSize = 16 * 1024 * 1024;
    if (setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize)) == -1) {
        utils::logWarning("Failed to set SO_SNDBUF");
    }
    
    // Configure server address
    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(port_);
    
    if (inet_pton(AF_INET, host_.c_str(), &serverAddr_.sin_addr) <= 0) {
        utils::logError("Invalid server address: " + host_);
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    return true;
}

bool TCPReceiver::connect() {
    if (!setupConnection()) {
        return false;
    }
    
    // Connect to server
    if (::connect(clientSocket_, (struct sockaddr*)&serverAddr_, sizeof(serverAddr_)) == -1) {
        utils::logError("Failed to connect to server " + host_ + ":" + std::to_string(port_));
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    // Connected to server successfully
    
    // Send START_STREAMING signal
    const char* signal = "START_STREAMING";
    ssize_t bytesSent = write(clientSocket_, signal, strlen(signal));
    if (bytesSent != static_cast<ssize_t>(strlen(signal))) {
        utils::logError("Failed to send START_STREAMING signal");
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    // START_STREAMING signal sent
    connected_ = true;
    return true;
}

void TCPReceiver::startReceiving() {
    if (!connected_) {
        utils::logError("Not connected to server");
        return;
    }
    
    if (receiving_) {
        utils::logWarning("Receiving already in progress");
        return;
    }
    
    if (!orderBook_) {
        utils::logError("No order book set");
        return;
    }
    
    // JSON generation happens in separate thread (jsonGenerationThread_)
    
    // Open JSON file once (append mode)
    if (!jsonOutputFile_.empty()) {
        jsonFile_.open(jsonOutputFile_, std::ios::app);
        if (!jsonFile_.is_open()) {
            utils::logWarning("Failed to open JSON output file; JSON will not be persisted");
        }
    }

    receiving_ = true;
    receivingThread_ = std::jthread([this](std::stop_token st){ this->receivingLoop(st); });
    jsonGenerationThread_ = std::jthread([this](std::stop_token st){ this->jsonGenerationLoop(st); });
}

void TCPReceiver::stopReceiving() {
    // Set receiving flag to false
    receiving_ = false;
    // Request stops to unblock any waits
    if (receivingThread_.joinable()) {
        // Shutdown socket to unblock read immediately
        if (clientSocket_ != -1) {
            shutdown(clientSocket_, SHUT_RD);
        }
        receivingThread_.request_stop();
        receivingThread_.join();
    }
    if (jsonRingBuffer_ && jsonGenerationThread_.joinable()) {
        jsonGenerationThread_.request_stop();
        // Wake any waiting consumer
        jsonRingBuffer_->notify_all();
        jsonGenerationThread_.join();
    }
    
    // CRITICAL: Always flush remaining JSON data
    flushJsonBuffer();
    
    // One final flush to ensure all data is written and close file
    flushJsonBuffer();
    if (jsonFile_.is_open()) {
        jsonFile_.flush();
        jsonFile_.close();
    }
    
    // Stats printing moved to receiver_main.cpp
    
    // Close socket
    if (clientSocket_ != -1) {
        close(clientSocket_);
        clientSocket_ = -1;
    }
    
    // Mark as disconnected
    connected_ = false;
}

void TCPReceiver::receivingLoop(std::stop_token stopToken) {
    // Circular byte buffer to avoid memmove
    static constexpr size_t kBufSize = 128 * 1024;
    std::array<char, kBufSize> buffer{};
    size_t head = 0; // read position
    size_t tail = 0; // write position
    auto buf_size = [&]() {
        if (tail >= head) return tail - head;
        return kBufSize - (head - tail);
    };
    auto buf_free = [&]() {
        return kBufSize - buf_size() - 1; // leave 1 byte to distinguish full
    };
    bool timingStarted = false;
    
    try {
        while (!stopToken.stop_requested()) {
            // Read raw bytes from network into circular buffer
            size_t free_bytes = buf_free();
            if (free_bytes == 0) {
                // Should not happen often; yield briefly
                std::this_thread::yield();
                continue;
            }
            // Determine contiguous space at tail
            size_t contiguous = (tail >= head) ? (kBufSize - tail) : (head - tail - 1);
            if (contiguous == 0) {
                tail = (tail + 0) % kBufSize;
                continue;
            }
            std::span<char> writeSpan{buffer.data() + tail, contiguous};
            ssize_t bytesReceived = read(clientSocket_, writeSpan.data(), writeSpan.size());
            
            if (bytesReceived <= 0) {
                if (bytesReceived != 0) {
                    utils::logError("Error receiving data");
                }
                // Natural end or error: signal disconnect and break
                connected_ = false;
                break;
            }
            
            tail = (tail + static_cast<size_t>(bytesReceived)) % kBufSize;

            // Process as many complete messages as possible from buffer
            while (buf_size() >= sizeof(MboMessage)) {
                // If message wraps, copy to a local aligned storage
                MboMessage localMsg;
                if (head + sizeof(MboMessage) <= kBufSize) {
                    std::span<const char> msgSpan{buffer.data() + head, sizeof(MboMessage)};
                    std::memcpy(&localMsg, msgSpan.data(), sizeof(MboMessage));
                } else {
                    size_t first_part = kBufSize - head;
                    std::span<const char> firstSpan{buffer.data() + head, first_part};
                    std::span<const char> secondSpan{buffer.data(), sizeof(MboMessage) - first_part};
                    std::memcpy(reinterpret_cast<char*>(&localMsg), firstSpan.data(), first_part);
                    std::memcpy(reinterpret_cast<char*>(&localMsg) + first_part, secondSpan.data(), sizeof(MboMessage) - first_part);
                }

                // Start timing on first message processed
                if (!timingStarted) {
                    startTime_ = std::chrono::high_resolution_clock::now();
                    timingStarted = true;
                }

                // Count message as received regardless of processing success
                receivedMessages_++;

                // Process message immediately
                try {
                    databento::MboMsg mbo = convertToDatabentoMbo(localMsg);
                    auto applyStart = std::chrono::high_resolution_clock::now();
                    {
                        std::unique_lock<std::shared_mutex> lock(orderBookMutex_);
                        orderBook_->Apply(mbo);
                    }
                    auto applyEnd = std::chrono::high_resolution_clock::now();
                    orderProcessTimesNs_.push_back((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(applyEnd - applyStart).count());
                    processedOrders_++;

                    // Push to ring buffer for JSON generation (always enabled)
                    if (jsonRingBuffer_) {
                        MboMessageWrapper wrapper(mbo);
                        // Blocking push to preserve order and avoid drops
                        jsonRingBuffer_->push(wrapper);
                    }

                } catch (const std::invalid_argument& e) {
                    // Handle missing orders/levels gracefully
                    std::string error_msg = e.what();
                    if (error_msg.find("No order with ID") != std::string::npos ||
                        error_msg.find("Received event for unknown level") != std::string::npos) {
                        // skip count (quiet)
                    } else {
                        throw;
                    }
                } catch (const std::exception& e) {
                    utils::logError("Error processing order: " + std::string(e.what()));
                }

                head = (head + sizeof(MboMessage)) % kBufSize;
            }
        }
    } catch (const std::exception& e) {
        utils::logError("Error during receiving: " + std::string(e.what()));
    }
    
    endTime_ = std::chrono::high_resolution_clock::now();
    // Message reception and order book processing completed
}

bool TCPReceiver::receiveData(void* data, size_t size) {
    ssize_t bytesReceived = read(clientSocket_, data, size);
    return bytesReceived == static_cast<ssize_t>(size);
}

databento::MboMsg TCPReceiver::convertToDatabentoMbo(const MboMessage& msg) {
    databento::MboMsg mbo;
    
    // Convert timestamps - databento uses time_since_epoch().count() for raw values
    // We need to reconstruct the time_point from the raw nanoseconds
    auto epoch_time = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(msg.ts_event)));
    mbo.hd.ts_event = epoch_time;
    
    auto recv_time = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(msg.ts_recv)));
    mbo.ts_recv = recv_time;
    
    // Convert header fields
    mbo.hd.rtype = static_cast<databento::RType>(msg.rtype);
    mbo.hd.publisher_id = msg.publisher_id;
    mbo.hd.instrument_id = msg.instrument_id;
    
    // Convert order fields
    mbo.action = static_cast<databento::Action>(msg.action);
    mbo.side = static_cast<databento::Side>(msg.side);
    mbo.price = msg.price;
    mbo.size = msg.size;
    mbo.channel_id = msg.channel_id;
    mbo.order_id = msg.order_id;
    mbo.flags = databento::FlagSet(msg.flags);
    mbo.ts_in_delta = std::chrono::nanoseconds(msg.ts_in_delta);
    mbo.sequence = msg.sequence;
    
    return mbo;
}

void TCPReceiver::jsonGenerationLoop(std::stop_token stopToken) {
    MboMessageWrapper wrapper;
    while (!stopToken.stop_requested() || !jsonRingBuffer_->empty()) {
        if (jsonRingBuffer_->try_pop(wrapper)) {
            try {
                std::string json = generateJsonOutput(wrapper.mbo);
                addJsonToBuffer(json);
                jsonOutputs_++;
            } catch (const std::exception& e) {
                utils::logError("Error generating JSON: " + std::string(e.what()));
            }
        } else {
            // Block until new data arrives or stop is requested
            if (stopToken.stop_requested()) {
                // Wake and retry drain condition
                jsonRingBuffer_->wait_for_data();
            } else {
                jsonRingBuffer_->wait_for_data();
            }
        }
    }
}

std::string TCPReceiver::generateJsonOutput(const db::MboMsg& mbo) {
    std::string json;
    json.reserve(512); // Pre-allocate reasonable size
    
    json += "{";
    if (!symbol_.empty()) {
        json += std::format("\"symbol\":\"{}\",", symbol_);
    }
    auto ts_ns = mbo.hd.ts_event.time_since_epoch().count();
    json += std::format("\"timestamp\":\"{}\",", ts_ns);
    json += std::format("\"timestamp_ns\":{},", ts_ns);
    if (orderBook_) {
        // Protect read access with shared lock to avoid data races
        std::shared_lock<std::shared_mutex> readLock(orderBookMutex_);
        auto [bid, ask] = orderBook_->Bbo();
        json += "\"bbo\":{";
        if (bid.price != databento::kUndefPrice) {
            json += std::format("\"bid\":{{\"price\":\"{}\",\"size\":{},\"count\":{}}}", 
                               std::to_string(bid.price), bid.size, bid.count);
        } else {
            json += "\"bid\":null";
        }
        json += ",";
        if (ask.price != databento::kUndefPrice) {
            json += std::format("\"ask\":{{\"price\":\"{}\",\"size\":{},\"count\":{}}}", 
                               std::to_string(ask.price), ask.size, ask.count);
        } else {
            json += "\"ask\":null";
        }
        json += "},";
        size_t bidCount = std::min(topLevels_, orderBook_->GetBidLevelCount());
        size_t askCount = std::min(topLevels_, orderBook_->GetAskLevelCount());
        json += "\"levels\":{";
        json += "\"bids\":[";
        for (size_t i = 0; i < bidCount; ++i) {
            auto bid = orderBook_->GetBidLevel(i);
            if (!bid || bid.price == databento::kUndefPrice) break;
            if (i > 0) json += ",";
            json += std::format("{{\"price\":\"{}\",\"size\":{},\"count\":{}}}", 
                               std::to_string(bid.price), bid.size, bid.count);
        }
        json += "],\"asks\":[";
        for (size_t i = 0; i < askCount; ++i) {
            auto ask = orderBook_->GetAskLevel(i);
            if (!ask || ask.price == databento::kUndefPrice) break;
            if (i > 0) json += ",";
            json += std::format("{{\"price\":\"{}\",\"size\":{},\"count\":{}}}", 
                               std::to_string(ask.price), ask.size, ask.count);
        }
        json += "]},";
        json += std::format("\"stats\":{{\"total_orders\":{},\"bid_levels\":{},\"ask_levels\":{}}}", 
                           orderBook_->GetOrderCount(), orderBook_->GetBidLevelCount(), orderBook_->GetAskLevelCount());
    }
    json += "}";
    return json;
}

double TCPReceiver::getThroughput() const {
    if (receivedMessages_ == 0) return 0.0;
    
    // Use endTime_ if available (after processing completed), otherwise use current time
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - startTime_);
    
    if (duration.count() == 0) return 0.0;
    
    return (double)receivedMessages_ * 1000.0 / duration.count();
}

double TCPReceiver::getAverageOrderProcessNs() const {
    if (orderProcessTimesNs_.empty()) return 0.0;
    uint64_t sumNs = std::accumulate(orderProcessTimesNs_.begin(), orderProcessTimesNs_.end(), uint64_t{0});
    return static_cast<double>(sumNs) / static_cast<double>(orderProcessTimesNs_.size());
}

uint64_t TCPReceiver::getP99OrderProcessNs() const {
    if (orderProcessTimesNs_.empty()) return 0;
    std::vector<uint64_t> timesCopy = orderProcessTimesNs_;
    size_t n = timesCopy.size();
    size_t idx = (n * 99 + 99) / 100; // ceil(0.99 * n)
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    std::nth_element(timesCopy.begin(), timesCopy.begin() + (idx - 1), timesCopy.end());
    return timesCopy[idx - 1];
}

void TCPReceiver::cleanup() {
    if (clientSocket_ != -1) {
        close(clientSocket_);
        clientSocket_ = -1;
    }
}

// JSON batching methods
void TCPReceiver::addJsonToBuffer(const std::string& json) {
    std::lock_guard<std::mutex> lock(jsonBufferMutex_);
    jsonBuffer_.push_back(json);
    
    size_t currentSize = jsonBuffer_.size();
    
    // Flush buffer when it reaches the batch size or flush interval
    if (currentSize >= jsonBatchSize_ || 
        (currentSize > 0 && currentSize % jsonFlushInterval_ == 0)) {
        flushJsonBufferInternal(); // Call internal version (assumes lock is held)
    }
}

void TCPReceiver::flushJsonBuffer() {
    std::lock_guard<std::mutex> lock(jsonBufferMutex_);
    flushJsonBufferInternal();
}

void TCPReceiver::flushJsonBufferInternal() {
    // This method assumes the mutex is already locked by the caller
    if (jsonBuffer_.empty()) {
        return;
    }
    
    // Write all buffered JSON strings to file at once
    if (jsonFile_.is_open()) {
        for (const auto& json : jsonBuffer_) {
            jsonFile_ << json << std::endl;
        }
        jsonFile_.flush();
    }
    
    // Clear the buffer
    jsonBuffer_.clear();
}
