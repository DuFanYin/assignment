#include "../include/tcp_receiver.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <databento/pretty.hpp>

TCPReceiver::TCPReceiver() 
    : host_("127.0.0.1"), port_(8080), orderBook_(nullptr), symbol_(""), 
      topLevels_(10), outputFullBook_(true), jsonOutputEnabled_(false),
      jsonBatchSize_(1000), jsonFlushInterval_(100), // Batch 1000 JSONs, flush every 100
      clientSocket_(-1), connected_(false), receiving_(false), 
      receivedMessages_(0), processedOrders_(0), jsonOutputs_(0) {
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
    
    // Try to resolve hostname to IP address
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(host_.c_str(), nullptr, &hints, &result);
    if (ret != 0) {
        utils::logError("Failed to resolve hostname: " + host_ + " - " + gai_strerror(ret));
        close(clientSocket_);
        clientSocket_ = -1;
        return false;
    }
    
    // Use the first resolved address
    struct sockaddr_in* addr_in = (struct sockaddr_in*)result->ai_addr;
    serverAddr_.sin_addr = addr_in->sin_addr;
    freeaddrinfo(result);
    
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
    
    // Configure order book for batched JSON output
    if (jsonOutputEnabled_) {
        orderBook_->setJsonCallback([this](const std::string& json) {
            addJsonToBuffer(json);
            // jsonOutputs_++; // Remove duplicate counting - already counted in order_book.hpp
        });
        orderBook_->setSymbol(symbol_);
        orderBook_->setTopLevels(topLevels_);
        orderBook_->setOutputFullBook(outputFullBook_);
        orderBook_->enableJsonOutput(true);
    }
    
    receiving_ = true;
    receivingThread_ = std::thread(&TCPReceiver::receivingLoop, this);
}

void TCPReceiver::stopReceiving() {
    // Set receiving flag to false
    receiving_ = false;
    
    // Wait for receiving thread to finish
    if (receivingThread_.joinable()) {
        receivingThread_.join();
    }
    
    // CRITICAL: Always flush remaining JSON data, even if thread already stopped
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
    // Use simple buffer approach like the old working implementation
    char buffer[4096];
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
                // Set flags to false so main thread knows we're done
                receiving_ = false;
                connected_ = false;
                break;
            }
            
            bufferPos += bytesReceived;
            
            // Process complete messages from buffer immediately
            while (bufferPos >= sizeof(MboMessage)) {
                MboMessage* msg = reinterpret_cast<MboMessage*>(buffer);
                
                // Start timing on first message processed
                if (!timingStarted) {
                    startTime_ = std::chrono::high_resolution_clock::now();
                    timingStarted = true;
                }
                
                // Process message immediately (no queuing, no ring buffer complexity)
                try {
                    databento::MboMsg mbo = convertToDatabentoMbo(*msg);
                    orderBook_->Apply(mbo);
                    processedOrders_++;
                    receivedMessages_++;
                    
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
                
                // Move remaining data to beginning of buffer (like old implementation)
                memmove(buffer, buffer + sizeof(MboMessage), bufferPos - sizeof(MboMessage));
                bufferPos -= sizeof(MboMessage);
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
        // Count actual records in the file for accurate reporting
        std::ifstream file(jsonOutputFile_);
        int actualRecords = 0;
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty()) {
                    actualRecords++;
                }
            }
            file.close();
        }
        std::cout << "JSON Records Generated: " << actualRecords << std::endl;
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
    
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime_);
    
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
