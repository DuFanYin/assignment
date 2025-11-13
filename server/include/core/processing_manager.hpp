#pragma once

#include "util/order_book.hpp"
#include "util/utils.hpp"

#include <databento/dbn_decoder.hpp>
#include <databento/record.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

class StreamBuffer;

namespace project {
class DatabaseWriter;
class JSONGenerator;
}

class PersistenceManager;

class ProcessingManager {
public:
    ProcessingManager(size_t topLevels,
                      std::atomic<bool>& serverRunningFlag);

    void attachPersistence(PersistenceManager* persistence);

    void startProcessing(const std::shared_ptr<StreamBuffer>& streamBuffer,
                         size_t expectedSize,
                         const std::string& fileName,
                         const std::function<void(const std::string&)>& sendMessage);
    void stopProcessing();

    bool isProcessing() const;

    const SessionStats& sessionStats() const { return sessionStats_; }
    size_t totalMessagesProcessed() const { return totalMessagesProcessed_.load(std::memory_order_relaxed); }
    double getThroughput() const;
    double getOrderThroughput() const;
    double getAverageOrderProcessNs() const;
    uint64_t getP99OrderProcessNs() const;

    double getDbThroughput() const;
    const std::string& activeSessionId() const { return activeSessionId_; }
    const std::string& symbol() const { return symbol_; }
    std::chrono::steady_clock::time_point processingStartTime() const { return processingStartTime_; }
    std::chrono::steady_clock::time_point processingEndTime() const { return processingEndTime_; }
    std::chrono::steady_clock::time_point dbStartTime() const { return dbStartTime_; }
    std::chrono::steady_clock::time_point dbEndTime() const { return dbEndTime_; }

    void setUploadMetrics(std::chrono::steady_clock::time_point start,
                          std::chrono::steady_clock::time_point end,
                          size_t bytesReceived);

    double getUploadThroughputMBps() const;

private:
    void processDbnStream(const std::shared_ptr<StreamBuffer>& streamBuffer,
                          size_t expectedSize,
                          const std::string& fileName,
                          const std::function<void(const std::string&)>& sendMessage);

    void resetState();
    void updateTiming(uint64_t elapsedNs);
    void finalizeStats();

    static constexpr size_t kTimingReservoirSize = 8192;
    static constexpr size_t kStatusUpdateInterval = 1000;
    static constexpr size_t kPriceScaleFactor = 100;
    static constexpr int64_t kNanosToCents = 10000000;

    size_t topLevels_;
    std::atomic<bool>& isServerRunning_;
    std::atomic<size_t> totalMessagesProcessed_{0};

    PersistenceManager* persistence_{nullptr};

    Book orderBook_;
    SessionStats sessionStats_;

    std::optional<std::jthread> processingThread_;

    std::atomic<size_t> processingMessagesReceived_{0};
    std::atomic<size_t> processingOrdersProcessed_{0};
    uint64_t processingTotalTimeNs_{0};
    std::atomic<uint64_t> processingTimingSamples_{0};
    std::vector<uint64_t> processingTimingReservoir_;
    std::minstd_rand processingRng_;

    std::chrono::steady_clock::time_point processingStartTime_{};
    std::chrono::steady_clock::time_point processingEndTime_{};
    std::chrono::steady_clock::time_point uploadStartTime_{};
    std::chrono::steady_clock::time_point uploadEndTime_{};
    std::chrono::steady_clock::time_point dbStartTime_{};
    std::chrono::steady_clock::time_point dbEndTime_{};
    size_t uploadBytesReceived_{0};

    std::string symbol_;
    std::string activeSessionId_;
    double dbThroughput_{0.0};
};


