#include "core/persistence_manager.hpp"

#include "database/database_writer.hpp"
#include "database/json_generator.hpp"
#include "util/utils.hpp"

#include <thread>
#include <vector>

namespace {
constexpr std::chrono::milliseconds kDatabaseWriterSleep{1};
constexpr size_t kBatchSize = 50000;
}

PersistenceManager::PersistenceManager(const ClickHouseConnection::Config& config)
    : config_(config) {}

PersistenceManager::~PersistenceManager() {
    markProcessingComplete();
    waitForCompletion();
}

bool PersistenceManager::initialize() {
    if (databaseWriter_ && jsonGenerator_) {
        return true;
    }

    try {
        databaseWriter_ = std::make_unique<project::DatabaseWriter>(config_);
        jsonGenerator_ = std::make_unique<project::JSONGenerator>(config_);
    } catch (const std::exception& e) {
        utils::logError("Failed to initialize persistence components: " + std::string(e.what()));
        databaseWriter_.reset();
        jsonGenerator_.reset();
        return false;
    }

    return true;
}

void PersistenceManager::resetSession() {
    sessionStats_ = {};
    activeSessionId_.clear();
    dbThroughput_ = 0.0;
    itemsWritten_ = 0;
    dbStartTime_ = {};
    dbEndTime_ = {};
}

bool PersistenceManager::beginSession(const std::string& symbol,
                                      const std::string& fileName,
                                      size_t payloadSize) {
    if (!databaseWriter_) {
        utils::logError("Database writer unavailable; cannot begin session");
        return false;
    }

    resetSession();

    try {
        databaseWriter_->startSession(symbol, fileName, payloadSize);
        activeSessionId_ = databaseWriter_->getCurrentSessionId();
    } catch (const std::exception& e) {
        utils::logError("Failed to start database session: " + std::string(e.what()));
        return false;
    }

    return true;
}

void PersistenceManager::startWriter() {
    if (writerRunning_.load(std::memory_order_acquire)) {
        return;
    }

    processingActive_.store(true, std::memory_order_release);
    writerRunning_.store(true, std::memory_order_release);

    databaseWriterThread_ = std::jthread([this](std::stop_token stopToken) {
        databaseWriterLoop(stopToken);
    });
}

void PersistenceManager::enqueueSnapshot(const MboMessageWrapper& wrapper) {
    snapshotRingBuffer_.push(wrapper);
}

void PersistenceManager::setSessionStats(const SessionStats& stats) {
    sessionStats_ = stats;
    std::atomic_thread_fence(std::memory_order_release);
}

void PersistenceManager::markProcessingComplete() {
    processingActive_.store(false, std::memory_order_release);
    snapshotRingBuffer_.notify_all();
}

void PersistenceManager::waitForCompletion() {
    if (databaseWriterThread_.joinable()) {
        databaseWriterThread_.join();
    }
    writerRunning_.store(false, std::memory_order_release);
}

void PersistenceManager::finalizeSessionSuccess() {
    // databaseWriterLoop performs the success finalization, so nothing needed here.
}

void PersistenceManager::finalizeSessionFailure(const std::string& error) {
    markProcessingComplete();
    waitForCompletion();
    if (databaseWriter_) {
        databaseWriter_->endSession(false, error);
    }
}

void PersistenceManager::databaseWriterLoop(std::stop_token stopToken) {
    if (!databaseWriter_) {
        utils::logError("Database writer loop started without an available writer");
        return;
    }

    databaseWriter_->dropIndexes();

    std::vector<MboMessageWrapper> batch;
    batch.reserve(kBatchSize);
    bool started = false;
    itemsWritten_ = 0;

    auto writeBatch = [&]() {
        if (batch.empty()) {
            return;
        }
        if (databaseWriter_->writeBatch(batch)) {
            itemsWritten_ += batch.size();
        }
        batch.clear();
    };

    MboMessageWrapper wrapper;

    while ((processingActive_.load(std::memory_order_acquire) || !snapshotRingBuffer_.empty()) &&
           !stopToken.stop_requested()) {
        if (snapshotRingBuffer_.try_pop(wrapper)) {
            if (!started) {
                started = true;
                dbStartTime_ = std::chrono::steady_clock::now();
            }
            batch.push_back(wrapper);
            if (batch.size() >= kBatchSize) {
                writeBatch();
            }
        } else {
            writeBatch();
            if ((!processingActive_.load(std::memory_order_acquire) && snapshotRingBuffer_.empty()) ||
                stopToken.stop_requested()) {
                break;
            }
            std::this_thread::sleep_for(kDatabaseWriterSleep);
        }
    }

    writeBatch();

    dbEndTime_ = std::chrono::steady_clock::now();

    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(dbEndTime_ - dbStartTime_).count();
    if (durationMs > 0) {
        dbThroughput_ = static_cast<double>(itemsWritten_) * 1000.0 / static_cast<double>(durationMs);
    } else {
        dbThroughput_ = 0.0;
    }

    databaseWriter_->recreateIndexes();

    std::atomic_thread_fence(std::memory_order_acquire);

    if (sessionStats_.messagesReceived > 0 || sessionStats_.ordersProcessed > 0) {
        databaseWriter_->updateSessionStats(
            sessionStats_.messagesReceived,
            sessionStats_.ordersProcessed,
            sessionStats_.throughput,
            sessionStats_.avgProcessNs,
            sessionStats_.p99ProcessNs
        );
    }

    if (sessionStats_.hasBookState) {
        databaseWriter_->updateFinalBookState(
            sessionStats_.totalOrders,
            sessionStats_.bidLevels,
            sessionStats_.askLevels,
            sessionStats_.bestBid,
            sessionStats_.bestAsk,
            sessionStats_.spread
        );
    }

    databaseWriter_->endSession(true);

    writerRunning_.store(false, std::memory_order_release);
}


