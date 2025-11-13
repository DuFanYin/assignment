#include "core/processing_manager.hpp"

#include "core/persistence_manager.hpp"
#include "util/streamer.hpp"
#include "util/utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace db = databento;

ProcessingManager::ProcessingManager(size_t topLevels,
                                     std::atomic<bool>& serverRunningFlag)
    : topLevels_(topLevels)
    , isServerRunning_(serverRunningFlag)
    , processingTimingReservoir_()
    , processingRng_(std::random_device{}()) {
    orderBook_.setTopLevels(topLevels_);
    processingTimingReservoir_.reserve(kTimingReservoirSize);
}

void ProcessingManager::attachPersistence(PersistenceManager* persistence) {
    persistence_ = persistence;
}

void ProcessingManager::resetState() {
    orderBook_.Clear();
    symbol_.clear();
    activeSessionId_.clear();
    sessionStats_ = {};

    processingMessagesReceived_.store(0, std::memory_order_relaxed);
    processingOrdersProcessed_.store(0, std::memory_order_relaxed);
    processingTotalTimeNs_ = 0;
    processingTimingSamples_.store(0, std::memory_order_relaxed);
    processingTimingReservoir_.clear();

    processingStartTime_ = {};
    processingEndTime_ = {};
    dbStartTime_ = {};
    dbEndTime_ = {};
    uploadStartTime_ = {};
    uploadEndTime_ = {};
    uploadBytesReceived_ = 0;

    dbThroughput_ = 0.0;
}

void ProcessingManager::startProcessing(const std::shared_ptr<StreamBuffer>& streamBuffer,
                                        size_t expectedSize,
                                        const std::string& fileName,
                                        const std::function<void(const std::string&)>& sendMessage) {
    if (!persistence_) {
        utils::logError("Persistence manager not attached; cannot start processing");
        return;
    }

    stopProcessing();

    resetState();
    isServerRunning_.store(true, std::memory_order_release);

    processingThread_.emplace([this, streamBuffer, expectedSize, fileName, sendMessage](std::stop_token) {
        processDbnStream(streamBuffer, expectedSize, fileName, sendMessage);
    });
}

void ProcessingManager::stopProcessing() {
    if (processingThread_.has_value() && processingThread_->joinable()) {
        processingThread_->join();
    }
    processingThread_.reset();
    isServerRunning_.store(false, std::memory_order_release);
}

bool ProcessingManager::isProcessing() const {
    return processingThread_.has_value();
}

void ProcessingManager::setUploadMetrics(std::chrono::steady_clock::time_point start,
                                         std::chrono::steady_clock::time_point end,
                                         size_t bytesReceived) {
    uploadStartTime_ = start;
    uploadEndTime_ = end;
    uploadBytesReceived_ = bytesReceived;
}

double ProcessingManager::getUploadThroughputMBps() const {
    auto zero_tp = std::chrono::steady_clock::time_point{};
    if (!(uploadEndTime_ > uploadStartTime_) || uploadStartTime_ == zero_tp) {
        return 0.0;
    }
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(uploadEndTime_ - uploadStartTime_);
    if (duration.count() == 0) {
        return 0.0;
    }
    const double bytes = static_cast<double>(uploadBytesReceived_);
    if (bytes <= 0.0) {
        return 0.0;
    }
    const double durationSec = static_cast<double>(duration.count()) / 1000.0;
    return (bytes / (1024.0 * 1024.0)) / durationSec;
}

void ProcessingManager::processDbnStream(const std::shared_ptr<StreamBuffer>& streamBuffer,
                                         size_t expectedSize,
                                         const std::string& fileName,
                                         const std::function<void(const std::string&)>& sendMessage) {
    if (!streamBuffer) {
        utils::logError("Stream buffer is null. Cannot process DBN stream.");
        isServerRunning_.store(false, std::memory_order_release);
        return;
    }

    if (!persistence_) {
        utils::logError("Persistence manager is not available.");
        isServerRunning_.store(false, std::memory_order_release);
        return;
    }

    try {
        auto reader = std::make_unique<StreamReader>(streamBuffer);
        db::DbnDecoder decoder(db::ILogReceiver::Default(), std::move(reader));

        auto metadata = decoder.DecodeMetadata();
        if (!metadata.symbols.empty()) {
            symbol_ = metadata.symbols[0];
            orderBook_.setSymbol(symbol_);
        } else {
            utils::logWarning("No symbols found in DBN file metadata");
            symbol_.clear();
        }

        const std::string sessionFileName = fileName.empty() ? "upload.dbn" : fileName;
        const std::size_t payloadSize = expectedSize > 0 ? expectedSize : streamBuffer->getTotalBytes();

        const std::string sessionSymbol = symbol_.empty() ? "UNKNOWN" : symbol_;
        if (!persistence_->beginSession(sessionSymbol, sessionFileName, payloadSize)) {
            utils::logError("Unable to begin persistence session");
            if (sendMessage) {
                nlohmann::json error;
                error["type"] = "error";
                error["error"] = "Failed to start database session";
                sendMessage(error.dump());
            }
            isServerRunning_.store(false, std::memory_order_release);
            return;
        }

        activeSessionId_ = persistence_->activeSessionId();
        persistence_->startWriter();

        processingMessagesReceived_.store(0, std::memory_order_relaxed);
        processingOrdersProcessed_.store(0, std::memory_order_relaxed);
        processingTotalTimeNs_ = 0;
        processingTimingSamples_.store(0, std::memory_order_relaxed);
        processingTimingReservoir_.clear();

        if (sendMessage) {
            nlohmann::json status;
            status["type"] = "stats";
            status["status"] = "Processing file (streaming)...";
            status["messagesProcessed"] = 0;
            sendMessage(status.dump());
        }

        processingStartTime_ = std::chrono::steady_clock::now();

        const db::Record* record;
        while ((record = decoder.DecodeRecord()) != nullptr) {
            if (record->RType() == db::RType::Mbo) {
                const auto& mbo = record->Get<db::MboMsg>();

                processingMessagesReceived_.fetch_add(1, std::memory_order_relaxed);

                try {
                    auto applyStart = std::chrono::steady_clock::now();

                    db::MboMsg normalizedMbo = mbo;
                    if (normalizedMbo.price != db::kUndefPrice && normalizedMbo.price != 0) {
                        normalizedMbo.price = normalizedMbo.price / kNanosToCents;
                    }

                    orderBook_.Apply(normalizedMbo);

                    BookSnapshot snap;
                    snap.symbol = symbol_;
                    snap.ts_ns = mbo.hd.ts_event.time_since_epoch().count();

                    auto bbo = orderBook_.Bbo();
                    snap.bid = bbo.first;
                    snap.ask = bbo.second;
                    snap.total_orders = orderBook_.GetOrderCount();
                    snap.bid_levels = orderBook_.GetBidLevelCount();
                    snap.ask_levels = orderBook_.GetAskLevelCount();

                    size_t bidCount = std::min(topLevels_, orderBook_.GetBidLevelCount());
                    size_t askCount = std::min(topLevels_, orderBook_.GetAskLevelCount());
                    snap.bids.reserve(bidCount);
                    snap.asks.reserve(askCount);

                    for (size_t i = 0; i < bidCount; ++i) {
                        auto lvl = orderBook_.GetBidLevel(i);
                        if (!lvl || lvl.price == db::kUndefPrice) break;
                        snap.bids.push_back(LevelEntry{lvl.price, lvl.size, lvl.count});
                    }

                    for (size_t i = 0; i < askCount; ++i) {
                        auto lvl = orderBook_.GetAskLevel(i);
                        if (!lvl || lvl.price == db::kUndefPrice) break;
                        snap.asks.push_back(LevelEntry{lvl.price, lvl.size, lvl.count});
                    }

                    auto applyEnd = std::chrono::steady_clock::now();
                    const uint64_t elapsedNs =
                        static_cast<uint64_t>(std::chrono::nanoseconds(applyEnd - applyStart).count());
                    updateTiming(elapsedNs);

                    processingOrdersProcessed_.fetch_add(1, std::memory_order_relaxed);

                    persistence_->enqueueSnapshot(MboMessageWrapper(snap));

                    totalMessagesProcessed_.fetch_add(1, std::memory_order_relaxed);

                    if (sendMessage && totalMessagesProcessed_.load(std::memory_order_relaxed) % kStatusUpdateInterval == 0) {
                        nlohmann::json stats;
                        stats["type"] = "stats";
                        stats["status"] = "Processing...";
                        stats["messagesProcessed"] = totalMessagesProcessed_.load(std::memory_order_relaxed);
                        sendMessage(stats.dump());
                    }
                } catch (const std::invalid_argument& e) {
                    std::string error_msg = e.what();
                    if (error_msg.find("No order with ID") != std::string::npos ||
                        error_msg.find("Received event for unknown level") != std::string::npos) {
                        // Quietly ignore expected inconsistencies
                    } else {
                        throw;
                    }
                } catch (const std::exception& e) {
                    utils::logError("Error processing order: " + std::string(e.what()));
                }
            }
        }

        processingEndTime_ = std::chrono::steady_clock::now();

        finalizeStats();
        persistence_->setSessionStats(sessionStats_);
        std::atomic_thread_fence(std::memory_order_release);
        persistence_->markProcessingComplete();
        persistence_->waitForCompletion();
        persistence_->finalizeSessionSuccess();

        dbThroughput_ = persistence_->getDbThroughput();
        dbStartTime_ = persistence_->dbStartTime();
        dbEndTime_ = persistence_->dbEndTime();

        if (sendMessage) {
            nlohmann::json complete;
            complete["type"] = "complete";
            complete["messagesReceived"] = processingMessagesReceived_.load(std::memory_order_relaxed);
            complete["ordersProcessed"] = processingOrdersProcessed_.load(std::memory_order_relaxed);
            complete["messagesProcessed"] = totalMessagesProcessed_.load(std::memory_order_relaxed);
            complete["bytesReceived"] = uploadBytesReceived_;
            complete["dbWritesPending"] = 0;
            complete["sessionId"] = activeSessionId_;

            auto zero_tp = std::chrono::steady_clock::time_point{};
            double totalDurationSec = 0.0;
            if (uploadStartTime_ > zero_tp && dbEndTime_ > zero_tp && dbEndTime_ > uploadStartTime_) {
                totalDurationSec = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - uploadStartTime_).count()
                ) / 1000.0;
            }
            double uploadDurationSec = 0.0;
            if (uploadEndTime_ > uploadStartTime_ && uploadStartTime_ > zero_tp) {
                uploadDurationSec = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(uploadEndTime_ - uploadStartTime_).count()
                ) / 1000.0;
            }
            double processingDurationSec = 0.0;
            if (processingEndTime_ > processingStartTime_) {
                processingDurationSec = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(processingEndTime_ - processingStartTime_).count()
                ) / 1000.0;
            }
            double dbDurationSec = 0.0;
            if (dbEndTime_ > dbStartTime_ && dbStartTime_ > zero_tp) {
                dbDurationSec = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - dbStartTime_).count()
                ) / 1000.0;
            }

            complete["totalThroughput"] = getThroughput();
            complete["totalDurationSec"] = totalDurationSec;
            complete["orderThroughput"] = getOrderThroughput();
            complete["processingDurationSec"] = processingDurationSec;
            complete["dbThroughput"] = dbThroughput_;
            complete["dbDurationSec"] = dbDurationSec;
            complete["uploadThroughputMBps"] = getUploadThroughputMBps();
            complete["uploadDurationSec"] = uploadDurationSec;

            double avgNs = getAverageOrderProcessNs();
            uint64_t p99Ns = getP99OrderProcessNs();
            if (avgNs > 0.0) {
                complete["averageOrderProcessNs"] = avgNs;
                complete["p99OrderProcessNs"] = p99Ns;
            }

            if (orderBook_.GetOrderCount() > 0) {
                auto finalBbo = orderBook_.Bbo();
                auto finalBid = finalBbo.first;
                auto finalAsk = finalBbo.second;
                if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
                    double bidValue = static_cast<double>(finalBid.price) / static_cast<double>(kPriceScaleFactor);
                    double askValue = static_cast<double>(finalAsk.price) / static_cast<double>(kPriceScaleFactor);
                    double spreadValue = std::abs(askValue - bidValue);

                    complete["activeOrders"] = orderBook_.GetOrderCount();
                    complete["bidPriceLevels"] = orderBook_.GetBidLevelCount();
                    complete["askPriceLevels"] = orderBook_.GetAskLevelCount();
                    complete["bestBid"] = bidValue;
                    complete["bestBidSize"] = finalBid.size;
                    complete["bestBidCount"] = finalBid.count;
                    complete["bestAsk"] = askValue;
                    complete["bestAskSize"] = finalAsk.size;
                    complete["bestAskCount"] = finalAsk.count;
                    complete["bidAskSpread"] = spreadValue;
                }
            }

            sendMessage(complete.dump());
        }

        orderBook_.Clear();
    } catch (const std::exception& e) {
        utils::logError("Error processing DBN stream: " + std::string(e.what()));
        persistence_->finalizeSessionFailure(e.what());
        if (sendMessage) {
            nlohmann::json error;
            error["type"] = "error";
            error["error"] = "Error processing DBN stream: " + std::string(e.what());
            sendMessage(error.dump());
        }
    }

    isServerRunning_.store(false, std::memory_order_release);
}

void ProcessingManager::updateTiming(uint64_t elapsedNs) {
    processingTotalTimeNs_ += elapsedNs;
    uint64_t samples = processingTimingSamples_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (processingTimingReservoir_.size() < kTimingReservoirSize) {
        processingTimingReservoir_.push_back(elapsedNs);
    } else {
        std::uniform_int_distribution<uint64_t> dist(0, samples - 1);
        const uint64_t idx = dist(processingRng_);
        if (idx < kTimingReservoirSize) {
            processingTimingReservoir_[static_cast<size_t>(idx)] = elapsedNs;
        }
    }
}

void ProcessingManager::finalizeStats() {
    sessionStats_.messagesReceived = processingMessagesReceived_.load(std::memory_order_relaxed);
    sessionStats_.ordersProcessed = processingOrdersProcessed_.load(std::memory_order_relaxed);
    sessionStats_.throughput = getThroughput();
    sessionStats_.avgProcessNs = static_cast<int64_t>(getAverageOrderProcessNs());
    sessionStats_.p99ProcessNs = getP99OrderProcessNs();

    auto finalBbo = orderBook_.Bbo();
    auto finalBid = finalBbo.first;
    auto finalAsk = finalBbo.second;
    if (finalBid.price != db::kUndefPrice && finalAsk.price != db::kUndefPrice) {
        sessionStats_.totalOrders = orderBook_.GetOrderCount();
        sessionStats_.bidLevels = orderBook_.GetBidLevelCount();
        sessionStats_.askLevels = orderBook_.GetAskLevelCount();
        sessionStats_.bestBid = static_cast<double>(finalBid.price) / static_cast<double>(kPriceScaleFactor);
        sessionStats_.bestAsk = static_cast<double>(finalAsk.price) / static_cast<double>(kPriceScaleFactor);
        sessionStats_.spread = std::abs(sessionStats_.bestAsk - sessionStats_.bestBid);
        sessionStats_.hasBookState = true;
    } else {
        sessionStats_.hasBookState = false;
    }
}

double ProcessingManager::getThroughput() const {
    if (processingMessagesReceived_.load(std::memory_order_relaxed) == 0) {
        return 0.0;
    }
    auto zero_tp = std::chrono::steady_clock::time_point{};
    if (uploadStartTime_ <= zero_tp || dbEndTime_ <= zero_tp || dbEndTime_ <= uploadStartTime_) {
        return 0.0;
    }
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - uploadStartTime_);
    if (duration.count() == 0) {
        return 0.0;
    }
    return static_cast<double>(processingMessagesReceived_.load(std::memory_order_relaxed)) * 1000.0 /
           static_cast<double>(duration.count());
}

double ProcessingManager::getOrderThroughput() const {
    if (processingOrdersProcessed_.load(std::memory_order_relaxed) == 0) {
        return 0.0;
    }
    if (!(processingEndTime_ > processingStartTime_)) {
        return 0.0;
    }
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(processingEndTime_ - processingStartTime_);
    if (duration.count() == 0) {
        return 0.0;
    }
    return static_cast<double>(processingOrdersProcessed_.load(std::memory_order_relaxed)) * 1000.0 /
           static_cast<double>(duration.count());
}

double ProcessingManager::getAverageOrderProcessNs() const {
    uint64_t samples = processingTimingSamples_.load(std::memory_order_relaxed);
    if (samples == 0) {
        return 0.0;
    }
    return static_cast<double>(processingTotalTimeNs_) / static_cast<double>(samples);
}

uint64_t ProcessingManager::getP99OrderProcessNs() const {
    if (processingTimingReservoir_.empty()) {
        return 0;
    }
    const size_t sampleCount =
        static_cast<size_t>(std::min<uint64_t>(processingTimingSamples_.load(std::memory_order_relaxed),
                                               processingTimingReservoir_.size()));
    if (sampleCount == 0) {
        return 0;
    }
    std::vector<uint64_t> samples;
    samples.reserve(sampleCount);
    samples.insert(samples.end(), processingTimingReservoir_.begin(),
                   processingTimingReservoir_.begin() + static_cast<std::ptrdiff_t>(sampleCount));

    size_t idx = (sampleCount * 99 + 99) / 100;
    if (idx == 0) idx = 1;
    if (idx > sampleCount) idx = sampleCount;
    std::nth_element(samples.begin(), samples.begin() + (idx - 1), samples.end());
    return samples[idx - 1];
}

double ProcessingManager::getDbThroughput() const {
    return dbThroughput_;
}


