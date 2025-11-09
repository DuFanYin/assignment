#pragma once

#include "database/postgres_connection.hpp"
#include "project/book_snapshot.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <vector>

// Forward declaration
struct MboMessageWrapper;

namespace project {

class DatabaseWriter {
public:
    explicit DatabaseWriter(const PostgresConnection::Config& config);
    ~DatabaseWriter();
    
    // Session management
    void startSession(const std::string& symbol, const std::string& fileName, size_t fileSize);
    void endSession(bool success, const std::string& errorMsg = "");
    void updateSessionStats(size_t messagesReceived, size_t ordersProcessed, 
                           double throughput, int64_t avgProcessNs, uint64_t p99ProcessNs);
    void updateFinalBookState(size_t totalOrders, size_t bidLevels, size_t askLevels,
                             double bestBid, double bestAsk, double spread);
    std::string getCurrentSessionId() const { return currentSessionId_; }
    
    // Write multiple snapshots in a batch (faster)
    bool writeBatch(const std::vector<MboMessageWrapper>& batch);
    
    // Index management for bulk operations
    bool dropIndexes();
    bool recreateIndexes();
    
private:
    PostgresConnection conn_;
    std::string currentSessionId_;
    std::atomic<bool> sessionActive_{false};
    std::atomic<size_t> snapshotsWritten_{0};
    
    // Prepared statements
    bool prepareStatements();
    void clearPreparedStatements();
    
    // Helper functions
    std::string generateSessionId() const;
    bool insertSession(const std::string& symbol, const std::string& fileName, size_t fileSize);
};

} // namespace project
