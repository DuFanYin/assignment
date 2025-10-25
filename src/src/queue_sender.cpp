#include "queue_sender.hpp"
#include "RingBuffer.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>

QueueSender::QueueSender() 
    : delayMs_(0), streaming_(false), sentMessages_(0) {
}

QueueSender::~QueueSender() {
    stopStreaming();
}

bool QueueSender::loadFromFile(const std::string& filePath) {
    try {
        store_ = std::make_unique<db::DbnFileStore>(filePath);
        utils::logInfo("Loaded file: " + filePath);
        return true;
    } catch (const std::exception& e) {
        utils::logError("Failed to load file: " + filePath + " - " + e.what());
        return false;
    }
}

void QueueSender::startStreaming() {
    if (streaming_) {
        utils::logWarning("Streaming already in progress");
        return;
    }
    
    if (!store_) {
        utils::logError("No file loaded");
        return;
    }
    
    if (!receiverQueue_) {
        utils::logError("No receiver queue set");
        return;
    }
    
    streaming_ = true;
    streamingThread_ = std::thread(&QueueSender::streamingLoop, this);
}

void QueueSender::stopStreaming() {
    if (!streaming_) {
        return;
    }
    
    streaming_ = false;
    if (streamingThread_.joinable()) {
        streamingThread_.join();
    }
}

void QueueSender::streamingLoop() {
    startTime_ = std::chrono::high_resolution_clock::now();
    
    try {
        while (streaming_) {
            const auto* record = store_->NextRecord();
            if (!record) break; // End of file
            
            if (record->RType() == db::RType::Mbo) {
                const auto& mbo = record->Get<db::MboMsg>();
                
                // Convert to lightweight message
                MboMessage message = convertToMboMessage(mbo);
                
                // Push to receiver queue
                while (streaming_ && !receiverQueue_->push(message)) {
                    // Queue is full, yield CPU
                    std::this_thread::yield();
                }
                
                if (!streaming_) break;
                
                sentMessages_++;
                
                // Optional delay
                if (delayMs_ > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
                }
            }
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
    }
    
    // Signal that streaming is complete
    streaming_ = false;
    
    endTime_ = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime_ - startTime_);
    
    // Final statistics
    std::cout << "\n=== Queue Sender Final Statistics ===" << std::endl;
    std::cout << "Streaming Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Messages Sent: " << sentMessages_ << std::endl;
    if (duration.count() > 0) {
        double messagesPerSecond = (double)sentMessages_ * 1000.0 / duration.count();
        std::cout << "Throughput: " << std::fixed << std::setprecision(0) << messagesPerSecond << " messages/sec" << std::endl;
    }
    std::cout << "===================================" << std::endl;
}

double QueueSender::getThroughput() const {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime_ - startTime_);
    if (duration.count() == 0) return 0.0;
    
    return (double)sentMessages_ * 1000.0 / duration.count();
}
