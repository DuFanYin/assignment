#pragma once

#include "database/clickhouse_connection.hpp"
#include "util/order_book.hpp"
#include "util/utils.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <vector>

namespace project {

class DatabaseWriter {
public:
    explicit DatabaseWriter(const ClickHouseConnection::Config& config);
    ~DatabaseWriter();
    
    // Session management
    void startSession(const std::string& symbol, const std::string& fileName, size_t fileSize);
    void endSession(bool success, const std::string& errorMsg = "");
    void updateSessionStats(size_t messagesReceived, size_t ordersProcessed, 
                           double throughput, int64_t avgProcessNs, uint64_t p99ProcessNs);
    void updateFinalBookState(size_t totalOrders, size_t bidLevels, size_t askLevels,
                             double bestBid, double bestAsk, double spread);
    std::string getCurrentSessionId() const { return activeSessionId_; }
    
    // Write multiple snapshots in a batch (ClickHouse native arrays - MUCH faster!)
    bool writeBatch(const std::vector<MboMessageWrapper>& batch);
    
    // ClickHouse doesn't need index management (sparse indexes are automatic)
    // Keeping these for compatibility but they're no-ops
    bool dropIndexes() { return true; }
    bool recreateIndexes() { return true; }
    
private:
    ClickHouseConnection clickhouseConnection_;
    std::string activeSessionId_;
    std::atomic<bool> isSessionActive_{false};
    std::atomic<size_t> totalSnapshotsWritten_{0};
    uint64_t nextId_{1};  // For generating snapshot IDs
    
    // Helper functions
    std::string generateSessionId() const;
    bool insertSession(const std::string& symbol, const std::string& fileName, size_t fileSize);
    void configureClientForBulkInserts();
};

} // namespace project
