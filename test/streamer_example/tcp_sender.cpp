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
#include <type_traits>
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
    : host_("127.0.0.1"), port_(8080), delayMs_(0),
      batchMode_(false), batchSize_(1000),
      streaming_(false), sentOrders_(0), connectedClients_(0), 
      fileDescriptor_(-1), mappedFile_(nullptr), fileSize_(0), serverSocket_(-1) {
}

TCPSender::~TCPSender() {
    stopStreaming();
    if (serverSocket_ != -1) {
        close(serverSocket_);
        serverSocket_ = -1;
    }
    if (mappedFile_ != nullptr && mappedFile_ != MAP_FAILED) {
        munmap(mappedFile_, fileSize_);
        mappedFile_ = nullptr;
    }
    if (fileDescriptor_ != -1) {
        close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    // Explicitly reset the store to ensure proper cleanup
    store_.reset();
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
    }
    if (streamingThread_.joinable()) {
        streamingThread_.join();
    }
    std::cout << "🛑 Stopped TCP streaming" << std::endl;
}

bool TCPSender::setupServer() {
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ == -1) {
        std::cerr << "❌ Failed to create socket" << std::endl;
        return false;
    }
    
    int opt = 1;
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "❌ Failed to set SO_REUSEADDR" << std::endl;
        close(serverSocket_);
        return false;
    }
    
    // Also set SO_REUSEPORT for better port reuse
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "⚠️  Failed to set SO_REUSEPORT (not critical)" << std::endl;
    }
    
    // Performance optimizations
    int nodelay = 1;
    if (setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1) {
        std::cerr << "⚠️  Failed to set TCP_NODELAY" << std::endl;
    }
    
    // Removed large socket buffer optimizations to prevent system errors
    
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(host_.c_str());
    serverAddr.sin_port = htons(port_);
    
    if (bind(serverSocket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "❌ Failed to bind socket to " << host_ << ":" << port_ << std::endl;
        std::cerr << "❌ Error: " << strerror(errno) << std::endl;
        std::cerr << "💡 Try: lsof -i :" << port_ << " (to check if port is in use)" << std::endl;
        std::cerr << "💡 Or wait a few seconds for the port to be released" << std::endl;
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
    // Removed CPU affinity and real-time scheduling optimizations to prevent crashes
    
    std::cout << "📡 Waiting for client connection..." << std::endl;
    
    // Wait for single client connection
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    
    int clientSocket = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientLen);
    if (clientSocket == -1) {
        std::cerr << "❌ Failed to accept client connection" << std::endl;
        return;
    }
    
    // Set performance options for client socket
    int nodelay = 1;
    if (setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) == -1) {
        std::cerr << "⚠️  Failed to set TCP_NODELAY on client socket" << std::endl;
    }
    
    // Removed large socket buffer optimizations to prevent system errors
    
    connectedClients_ = 1;
    std::cout << "🔗 Client connected from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
    
    // Wait for verification/start signal from client
    std::cout << "⏳ Waiting for start signal from client..." << std::endl;
    char buffer[1024];
    ssize_t bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytesReceived <= 0) {
        std::cerr << "❌ Failed to receive start signal from client" << std::endl;
        close(clientSocket);
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
    if (batchMode_) {
        std::cout << "🚀 Using BATCH STREAMING mode..." << std::endl;
        std::cout << "🚀 Pre-parsing entire file into memory for batch processing..." << std::endl;
        
        // Use simple byte-based approach - no alignment issues
        std::vector<std::vector<uint8_t>> allMessages;
        allMessages.reserve(1000000); // Reserve space for ~1M messages
        
        std::cout << "📊 Pre-parsing file into memory..." << std::endl;
        auto parseStart = std::chrono::high_resolution_clock::now();
        
        // Reset file to beginning
        store_ = std::make_unique<databento::DbnFileStore>(filePath_);
        
        while (true) {
            const auto* record = store_->NextRecord();
            if (!record) break;
            
            if (record->RType() == databento::RType::Mbo) {
                // Extract only the fields we need as raw bytes - no alignment issues
                const auto& mbo = record->template Get<databento::MboMsg>();
                
                // Create a simple byte array with the data we need
                std::vector<uint8_t> messageData;
                messageData.reserve(32); // Reserve space for our data
                
                // Add order_id (8 bytes)
                uint64_t order_id = mbo.order_id;
                for (int i = 0; i < 8; ++i) {
                    messageData.push_back(static_cast<uint8_t>(order_id >> (i * 8)));
                }
                
                // Add price (8 bytes)
                uint64_t price = static_cast<uint64_t>(mbo.price);
                for (int i = 0; i < 8; ++i) {
                    messageData.push_back(static_cast<uint8_t>(price >> (i * 8)));
                }
                
                // Add size (4 bytes)
                uint32_t size = mbo.size;
                for (int i = 0; i < 4; ++i) {
                    messageData.push_back(static_cast<uint8_t>(size >> (i * 8)));
                }
                
                // Add action (1 byte)
                messageData.push_back(static_cast<uint8_t>(mbo.action));
                
                // Add side (1 byte)
                messageData.push_back(static_cast<uint8_t>(mbo.side));
                
                allMessages.push_back(std::move(messageData));
            }
        }
        
        auto parseEnd = std::chrono::high_resolution_clock::now();
        auto parseTime = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart);
        std::cout << "✅ Pre-parsed " << allMessages.size() << " messages in " << parseTime.count() << "ms" << std::endl;
        
        // Get timestamp once for all messages
        auto baseTimestamp = std::chrono::high_resolution_clock::now();
        auto baseTime = std::chrono::duration_cast<std::chrono::microseconds>(baseTimestamp.time_since_epoch()).count();
        
        // Batch streaming loop - send messages in batches
        std::cout << "🚀 Starting batch streaming (batch size: " << batchSize_ << ")..." << std::endl;
        auto streamStart = std::chrono::high_resolution_clock::now();
        
        try {
            for (size_t i = 0; i < allMessages.size() && streaming_; i += batchSize_) {
                // Create batch
                size_t batchEnd = std::min(i + batchSize_, allMessages.size());
                std::vector<std::vector<uint8_t>> batch(allMessages.begin() + i, allMessages.begin() + batchEnd);
                
                // Send batch
                if (!sendBatchMessagesBytes(clientSocket, batch, baseTime + i)) {
                    std::cout << "🔌 Client disconnected during batch streaming" << std::endl;
                    break;
                }
                
                sentOrders_ += batch.size();
                
                // Optional delay between batches
                if (delayMs_ > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
                }
            }
            
            auto streamEnd = std::chrono::high_resolution_clock::now();
            auto streamTime = std::chrono::duration_cast<std::chrono::milliseconds>(streamEnd - streamStart);
            
            std::cout << "✅ Batch streaming completed!" << std::endl;
            if (streamTime.count() > 0) {
                auto messagesPerSecond = (allMessages.size() * 1000.0) / streamTime.count();
                std::cout << "📊 Performance: " << static_cast<size_t>(messagesPerSecond) << " messages/sec" << std::endl;
            } else {
                std::cout << "📊 Performance: Very fast (completed in < 1ms)" << std::endl;
            }
            std::cout << "📊 Batch size: " << batchSize_ << " messages per batch" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "❌ Error during batch streaming: " << e.what() << std::endl;
        }
    } else {
        std::cout << "🚀 Using SIMPLE STREAMING mode..." << std::endl;
        std::cout << "🚀 Pre-parsing entire file into memory for simple processing..." << std::endl;
        
        // Use simple byte-based approach - no alignment issues
        std::vector<std::vector<uint8_t>> allMessages;
        allMessages.reserve(1000000); // Reserve space for ~1M messages
        
        std::cout << "📊 Pre-parsing file into memory..." << std::endl;
        auto parseStart = std::chrono::high_resolution_clock::now();
        
        // Reset file to beginning
        store_ = std::make_unique<databento::DbnFileStore>(filePath_);
        
        while (true) {
            const auto* record = store_->NextRecord();
            if (!record) break;
            
            if (record->RType() == databento::RType::Mbo) {
                // Extract only the fields we need as raw bytes - no alignment issues
                const auto& mbo = record->template Get<databento::MboMsg>();
                
                // Create a simple byte array with the data we need
                std::vector<uint8_t> messageData;
                messageData.reserve(32); // Reserve space for our data
                
                // Add order_id (8 bytes)
                uint64_t order_id = mbo.order_id;
                for (int i = 0; i < 8; ++i) {
                    messageData.push_back(static_cast<uint8_t>(order_id >> (i * 8)));
                }
                
                // Add price (8 bytes)
                uint64_t price = static_cast<uint64_t>(mbo.price);
                for (int i = 0; i < 8; ++i) {
                    messageData.push_back(static_cast<uint8_t>(price >> (i * 8)));
                }
                
                // Add size (4 bytes)
                uint32_t size = mbo.size;
                for (int i = 0; i < 4; ++i) {
                    messageData.push_back(static_cast<uint8_t>(size >> (i * 8)));
                }
                
                // Add action (1 byte)
                messageData.push_back(static_cast<uint8_t>(mbo.action));
                
                // Add side (1 byte)
                messageData.push_back(static_cast<uint8_t>(mbo.side));
                
                allMessages.push_back(std::move(messageData));
            }
        }
        
        auto parseEnd = std::chrono::high_resolution_clock::now();
        auto parseTime = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart);
        std::cout << "✅ Pre-parsed " << allMessages.size() << " messages in " << parseTime.count() << "ms" << std::endl;
        
        // Note: No timestamp needed in simple mode since we don't send timestamps
        
        // Simple streaming loop - individual messages
        std::cout << "🚀 Starting simple streaming (individual messages)..." << std::endl;
        auto streamStart = std::chrono::high_resolution_clock::now();
        
        try {
            for (size_t i = 0; i < allMessages.size() && streaming_; ++i) {
                const auto& messageData = allMessages[i];
                
                // Send individual message as bytes
                if (!sendData(clientSocket, messageData.data(), messageData.size())) {
                    std::cout << "🔌 Client disconnected during streaming" << std::endl;
                    break;
                }
                
                sentOrders_++;
            }
            
            auto streamEnd = std::chrono::high_resolution_clock::now();
            auto streamTime = std::chrono::duration_cast<std::chrono::milliseconds>(streamEnd - streamStart);
            
            std::cout << "✅ Simple streaming completed!" << std::endl;
            if (streamTime.count() > 0) {
                auto messagesPerSecond = (allMessages.size() * 1000.0) / streamTime.count();
                std::cout << "📊 Performance: " << static_cast<size_t>(messagesPerSecond) << " messages/sec" << std::endl;
            } else {
                std::cout << "📊 Performance: Very fast (completed in < 1ms)" << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "❌ Error during simple streaming: " << e.what() << std::endl;
        }
    }
    
    if (clientSocket != -1) {
        close(clientSocket);
    }
    connectedClients_ = 0;
    std::cout << "✅ Streaming completed" << std::endl;
}



bool TCPSender::sendData(int socket, const void* data, size_t size) {
    ssize_t bytesSent = write(socket, data, size);
    return bytesSent == static_cast<ssize_t>(size);
}




bool TCPSender::sendBatchMessagesBytes(int clientSocket, const std::vector<std::vector<uint8_t>>& messages, uint64_t baseTimestamp) {
    if (messages.empty()) {
        return true;
    }
    
    // Calculate total batch size
    size_t headerSize = 16; // batchSize(4) + baseTimestamp(8) + reserved(4)
    size_t totalSize = headerSize;
    for (const auto& msg : messages) {
        totalSize += msg.size();
    }
    
    // Use simple buffer - no alignment concerns
    std::vector<uint8_t> batchBuffer;
    batchBuffer.reserve(totalSize);
    
    // Write batch header as bytes
    uint32_t batchSize = static_cast<uint32_t>(messages.size());
    for (int i = 0; i < 4; ++i) {
        batchBuffer.push_back(static_cast<uint8_t>(batchSize >> (i * 8)));
    }
    
    // Write base timestamp as bytes
    for (int i = 0; i < 8; ++i) {
        batchBuffer.push_back(static_cast<uint8_t>(baseTimestamp >> (i * 8)));
    }
    
    // Write reserved field (4 bytes)
    for (int i = 0; i < 4; ++i) {
        batchBuffer.push_back(0);
    }
    
    // Write all messages
    for (const auto& msg : messages) {
        batchBuffer.insert(batchBuffer.end(), msg.begin(), msg.end());
    }
    
    // Send entire batch
    return sendData(clientSocket, batchBuffer.data(), batchBuffer.size());
}
