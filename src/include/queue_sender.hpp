#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>
#include <databento/dbn_file_store.hpp>
#include <databento/record.hpp>
#include <databento/enums.hpp>
#include <databento/constants.hpp>
#include "message_types.hpp"

// Forward declaration
template<class T> class RingBuffer;

namespace db = databento;

class QueueSender {
public:
    QueueSender();
    ~QueueSender();

    // Configuration
    void setDelayMs(int delay) { delayMs_ = delay; }
    
    // Queue interface - receiver will call this to register its queue
    void setReceiverQueue(std::shared_ptr<RingBuffer<MboMessage>> queue) { 
        receiverQueue_ = queue; 
    }

    // Data loading and processing
    bool loadFromFile(const std::string& filePath);
    void startStreaming();
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // Statistics
    size_t getSentMessages() const { return sentMessages_; }
    double getThroughput() const;

private:
    // Configuration
    int delayMs_;
    
    // File handling
    std::unique_ptr<db::DbnFileStore> store_;
    
    // State
    std::atomic<bool> streaming_;
    std::atomic<size_t> sentMessages_;
    std::thread streamingThread_;
    
    // Timing
    std::chrono::high_resolution_clock::time_point startTime_;
    std::chrono::high_resolution_clock::time_point endTime_;
    
    // Queue interface
    std::shared_ptr<RingBuffer<MboMessage>> receiverQueue_;
    
    // Methods
    void streamingLoop();
};
