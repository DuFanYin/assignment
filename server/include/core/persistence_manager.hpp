#pragma once

#include "database/clickhouse_connection.hpp"
#include "util/ring_buffer.hpp"
#include "util/utils.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace project {
class DatabaseWriter;
class JSONGenerator;
}

class PersistenceManager {
public:
    explicit PersistenceManager(const ClickHouseConnection::Config& config);
    ~PersistenceManager();

    bool initialize();

    project::JSONGenerator* jsonGenerator() const { return jsonGenerator_.get(); }

    bool beginSession(const std::string& symbol,
                      const std::string& fileName,
                      size_t payloadSize);

    void startWriter();
    void enqueueSnapshot(const MboMessageWrapper& wrapper);
    void setSessionStats(const SessionStats& stats);
    void markProcessingComplete();
    void waitForCompletion();
    void finalizeSessionSuccess();
    void finalizeSessionFailure(const std::string& error);

    const std::string& activeSessionId() const { return activeSessionId_; }
    double getDbThroughput() const { return dbThroughput_; }
    std::chrono::steady_clock::time_point dbStartTime() const { return dbStartTime_; }
    std::chrono::steady_clock::time_point dbEndTime() const { return dbEndTime_; }

private:
    void databaseWriterLoop(std::stop_token stopToken);
    void resetSession();

    ClickHouseConnection::Config config_;
    std::unique_ptr<project::DatabaseWriter> databaseWriter_;
    std::unique_ptr<project::JSONGenerator> jsonGenerator_;

    RingBuffer<MboMessageWrapper> snapshotRingBuffer_;
    std::jthread databaseWriterThread_;
    std::atomic<bool> processingActive_{false};
    std::atomic<bool> writerRunning_{false};

    SessionStats sessionStats_;
    std::string activeSessionId_;
    double dbThroughput_{0.0};
    size_t itemsWritten_{0};

    std::chrono::steady_clock::time_point dbStartTime_{};
    std::chrono::steady_clock::time_point dbEndTime_{};
};


