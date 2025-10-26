#include "../include/tcp_receiver.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <fstream>
#include <databento/pretty.hpp>
#include <databento/constants.hpp>

TCPReceiver::TCPReceiver() 
    : host_("127.0.0.1"), port_(8080), orderBook_(nullptr), symbol_(""), 
      topLevels_(10), outputFullBook_(true), jsonOutputEnabled_(false),
      jsonBatchSize_(1000), jsonFlushInterval_(100), // Batch 1000 JSONs, flush every 100
      clientSocket_(-1), connected_(false), receiving_(false), 
      receivedMessages_(0), processedOrders_(0), jsonOutputs_(0),
      jsonRingBuffer_(std::make_unique<RingBuffer<MboMessageWrapper>>(65536)) {  // 64K ring buffer
    // Initialize JSON buffer
    jsonBuffer_.reserve(jsonBatchSize_);
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
    
    // Configure order book - DISABLE JSON callback to avoid blocking
    // JSON will be generated in separate thread from ring buffer
    orderBook_->enableJsonOutput(false);  // Don't generate JSON in Apply()
    
    receiving_ = true;
    receivingThread_ = std::thread(&TCPReceiver::receivingLoop, this);
    
    // Start JSON generation thread if JSON output is enabled
    if (jsonOutputEnabled_) {
        jsonGenerationThread_ = std::thread(&TCPReceiver::jsonGenerationLoop, this);
    }
}

void TCPReceiver::stopReceiving() {
    // Set receiving flag to false
    receiving_ = false;
    
    // Wait for receiving thread to finish
    if (receivingThread_.joinable()) {
        receivingThread_.join();
    }
    
    // Wait for JSON generation thread to finish processing ring buffer
    if (jsonGenerationThread_.joinable()) {
        // Give it time to drain the ring buffer
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        jsonGenerationThread_.join();
    }
    
    // CRITICAL: Always flush remaining JSON data
    flushJsonBuffer();
    
    // Force one more flush to ensure all data is written
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    flushJsonBuffer();
    
    // Close socket
    if (clientSocket_ != -1) {
        close(clientSocket_);
        clientSocket_ = -1;
    }
    
    // Mark as disconnected
    connected_ = false;
}

void TCPReceiver::receivingLoop() {
    // CRITICAL FIX: Use much larger buffer to reduce memmove frequency
    // Original: 4KB buffer -> memmove after every ~30-50 messages  
    // Optimized: 128KB buffer -> memmove after every ~2000 messages
    // This dramatically reduces the expensive memmove operations
    char buffer[128 * 1024];  // 128KB buffer - 64x larger than original
    size_t bufferPos = 0;
    bool timingStarted = false;
    
    try {
        while (receiving_) {
            // Read raw bytes from network into buffer
            ssize_t bytesReceived = read(clientSocket_, buffer + bufferPos, sizeof(buffer) - bufferPos);
            
            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    utils::logInfo("Server closed connection");
                } else {
                    utils::logError("Error receiving data");
                }
                receiving_ = false;
                connected_ = false;
                break;
            }
            
            bufferPos += bytesReceived;
            
            // Process complete messages from buffer
            while (bufferPos >= sizeof(MboMessage)) {
                MboMessage* msg = reinterpret_cast<MboMessage*>(buffer);
                
                // Start timing on first message processed
                if (!timingStarted) {
                    startTime_ = std::chrono::high_resolution_clock::now();
                    timingStarted = true;
                }
                
                // Process message immediately
                try {
                    databento::MboMsg mbo = convertToDatabentoMbo(*msg);
                    orderBook_->Apply(mbo);
                    processedOrders_++;
                    receivedMessages_++;
                    
                    // Push to ring buffer for JSON generation (in separate thread)
                    // This prevents blocking TCP receiving on slow JSON generation
                    if (jsonOutputEnabled_ && jsonRingBuffer_) {
                        MboMessageWrapper wrapper(mbo);
                        if (!jsonRingBuffer_->try_push(wrapper)) {
                            // Ring buffer full - skip this JSON (non-critical)
                            static size_t skippedJson = 0;
                            if (++skippedJson % 10000 == 0) {
                                utils::logInfo("Ring buffer full, skipped " + std::to_string(skippedJson) + " JSON messages");
                            }
                        }
                    }
                    
                } catch (const std::invalid_argument& e) {
                    // Handle missing orders/levels gracefully
                    std::string error_msg = e.what();
                    if (error_msg.find("No order with ID") != std::string::npos ||
                        error_msg.find("Received event for unknown level") != std::string::npos) {
                        static int skippedCount = 0;
                        if (++skippedCount % 1000 == 0) {
                            utils::logInfo("Skipped " + std::to_string(skippedCount) + " orders due to missing references (normal for real market data)");
                        }
                    } else {
                        throw;
                    }
                } catch (const std::exception& e) {
                    utils::logError("Error processing order: " + std::string(e.what()));
                }
                
                // Move remaining data to beginning of buffer
                // With large buffer, this happens much less frequently than with 4KB buffer
                bufferPos -= sizeof(MboMessage);
                if (bufferPos > 0) {
                    memmove(buffer, buffer + sizeof(MboMessage), bufferPos);
                }
            }
        }
    } catch (const std::exception& e) {
        utils::logError("Error during receiving: " + std::string(e.what()));
    }
    
    endTime_ = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime_ - startTime_);
    
    // Final statistics - consolidated output
    std::cout << "\n=== TCP Receiver Final Statistics ===" << std::endl;
    std::cout << "Processing Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Messages Received: " << receivedMessages_ << std::endl;
    std::cout << "Orders Processed: " << processedOrders_ << std::endl;
    if (jsonOutputEnabled_) {
        std::cout << "JSON Records Generated: " << orderBook_->getJsonOutputs() << std::endl;
    }
    if (duration.count() > 0) {
        double messagesPerSecond = (double)receivedMessages_ * 1000.0 / duration.count();
        double ordersPerSecond = (double)processedOrders_ * 1000.0 / duration.count();
        std::cout << "Message Throughput: " << std::fixed << std::setprecision(0) << messagesPerSecond << " messages/sec" << std::endl;
        std::cout << "Order Processing Rate: " << std::fixed << std::setprecision(0) << ordersPerSecond << " orders/sec" << std::endl;
    }
    
    // Order book final state
    if (orderBook_) {
        std::cout << "\nFinal Order Book Summary:" << std::endl;
        std::cout << "  Active Orders: " << orderBook_->GetOrderCount() << std::endl;
        std::cout << "  Bid Price Levels: " << orderBook_->GetBidLevelCount() << std::endl;
        std::cout << "  Ask Price Levels: " << orderBook_->GetAskLevelCount() << std::endl;
        
        auto finalBbo = orderBook_->Bbo();
        auto finalBid = finalBbo.first.price < finalBbo.second.price ? finalBbo.first : finalBbo.second;
        auto finalAsk = finalBbo.first.price > finalBbo.second.price ? finalBbo.first : finalBbo.second;
        
        std::cout << "  Best Bid: " << databento::pretty::Px{finalBid.price} << " @ " << finalBid.size << " (" << finalBid.count << " orders)" << std::endl;
        std::cout << "  Best Ask: " << databento::pretty::Px{finalAsk.price} << " @ " << finalAsk.size << " (" << finalAsk.count << " orders)" << std::endl;
        std::cout << "  Bid-Ask Spread: " << (finalAsk.price - finalBid.price) << std::endl;
    }
    std::cout << "=====================================" << std::endl;
    
    // Message reception and order book processing completed
}

void TCPReceiver::jsonGenerationLoop() {
    // This thread runs separately and generates JSON from ring buffer
    // This decouples slow JSON generation from fast TCP receiving
    
    MboMessageWrapper wrapper;
    size_t jsonCount = 0;
    
    while (receiving_ || !jsonRingBuffer_->empty()) {
        // Try to pop message from ring buffer
        if (jsonRingBuffer_->try_pop(wrapper)) {
            try {
                // Generate JSON from current order book state
                std::string json = generateJsonOutput(wrapper.mbo);
                addJsonToBuffer(json);
                jsonOutputs_++;
                jsonCount++;
                
            } catch (const std::exception& e) {
                utils::logError("Error generating JSON: " + std::string(e.what()));
            }
        } else {
            // Ring buffer empty, wait a bit before checking again
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    utils::logInfo("JSON generation thread processed " + std::to_string(jsonCount) + " messages");
}

std::string TCPReceiver::generateJsonOutput(const db::MboMsg& mbo) {
    // Generate JSON for order book state
    std::stringstream json;
    json << "{";
    
    // Add symbol if set
    if (!symbol_.empty()) {
        json << "\"symbol\":\"" << symbol_ << "\",";
    }
    
    // Add timestamp
    json << "\"timestamp\":\"" << std::to_string(mbo.hd.ts_event.time_since_epoch().count()) << "\",";
    json << "\"timestamp_ns\":" << mbo.hd.ts_event.time_since_epoch().count() << ",";
    
    // Add best bid/ask
    if (orderBook_) {
        auto bbo = orderBook_->Bbo();
        json << "\"bbo\":{";
        if (bbo.first.price != db::kUndefPrice) {
            json << "\"bid\":{\"price\":\"" << std::to_string(bbo.first.price) << "\",\"size\":" << bbo.first.size << ",\"count\":" << bbo.first.count << "}";
        } else {
            json << "\"bid\":null";
        }
        json << ",";
        if (bbo.second.price != db::kUndefPrice) {
            json << "\"ask\":{\"price\":\"" << std::to_string(bbo.second.price) << "\",\"size\":" << bbo.second.size << ",\"count\":" << bbo.second.count << "}";
        } else {
            json << "\"ask\":null";
        }
        json << "},";
        
        // Add top levels
        json << "\"levels\":{";
        json << "\"bids\":[";
        for (size_t i = 0; i < topLevels_; ++i) {
            auto bid = orderBook_->GetBidLevel(i);
            if (bid.price == db::kUndefPrice) break;
            if (i > 0) json << ",";
            json << "{\"price\":\"" << std::to_string(bid.price) << "\",\"size\":" << bid.size << ",\"count\":" << bid.count << "}";
        }
        json << "],\"asks\":[";
        for (size_t i = 0; i < topLevels_; ++i) {
            auto ask = orderBook_->GetAskLevel(i);
            if (ask.price == db::kUndefPrice) break;
            if (i > 0) json << ",";
            json << "{\"price\":\"" << std::to_string(ask.price) << "\",\"size\":" << ask.size << ",\"count\":" << ask.count << "}";
        }
        json << "]},";
        
        // Add statistics
        json << "\"stats\":{";
        json << "\"total_orders\":" << orderBook_->GetOrderCount() << ",";
        json << "\"bid_levels\":" << orderBook_->GetBidLevelCount() << ",";
        json << "\"ask_levels\":" << orderBook_->GetAskLevelCount();
        json << "}";
    }
    
    json << "}";
    return json.str();
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

double TCPReceiver::getThroughput() const {
    if (receivedMessages_ == 0) return 0.0;
    
    // Use endTime_ if available (after processing completed), otherwise use current time
    auto endTimeToUse = (endTime_ > startTime_) ? endTime_ : std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTimeToUse - startTime_);
    
    if (duration.count() == 0) return 0.0;
    
    return (double)receivedMessages_ * 1000.0 / duration.count();
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
    if (!jsonOutputFile_.empty()) {
        std::ofstream file(jsonOutputFile_, std::ios::app);
        if (file.is_open()) {
            for (const auto& json : jsonBuffer_) {
                file << json << std::endl;
            }
            file.flush(); // Force flush to disk
            file.close();
        }
    }
    
    // Clear the buffer
    jsonBuffer_.clear();
}
