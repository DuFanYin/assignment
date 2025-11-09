#include "database/database_writer.hpp"
#include "project/utils.hpp"
#include "project/server.hpp"  // For MboMessageWrapper
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <ctime>

namespace project {

DatabaseWriter::DatabaseWriter(const PostgresConnection::Config& config)
    : postgresConnection_(config) {
    if (!postgresConnection_.connect()) {
        throw std::runtime_error("Failed to connect to PostgreSQL database");
    }
    
    if (!prepareStatements()) {
        throw std::runtime_error("Failed to prepare SQL statements");
    }
}

DatabaseWriter::~DatabaseWriter() {
    clearPreparedStatements();
    postgresConnection_.disconnect();
}

std::string DatabaseWriter::generateSessionId() const {
    // Generate session ID: timestamp + random component
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    std::stringstream ss;
    ss << "session_" << ms << "_" << dis(gen);
    return ss.str();
}

bool DatabaseWriter::prepareStatements() {
    // No prepared statements needed - using COPY commands for bulk loading
    return true;
}

void DatabaseWriter::clearPreparedStatements() {
    // No prepared statements to clear - using COPY commands for bulk loading
}

void DatabaseWriter::startSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    activeSessionId_ = generateSessionId();
    
    if (!insertSession(symbol, fileName, fileSize)) {
        throw std::runtime_error("Failed to create database session");
    }
    
    isSessionActive_ = true;
    totalSnapshotsWritten_ = 0;
}

bool DatabaseWriter::insertSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    std::string sql = "INSERT INTO processing_sessions "
                     "(session_id, symbol, file_name, file_size, status) "
                     "VALUES ('" + postgresConnection_.escapeString(activeSessionId_) + "', '" + 
                     postgresConnection_.escapeString(symbol) + "', '" + 
                     postgresConnection_.escapeString(fileName) + "', " + std::to_string(fileSize) + ", 'processing')";
    
    auto result = postgresConnection_.execute(sql);
    return result.success;
}

void DatabaseWriter::endSession(bool success, const std::string& errorMsg) {
    if (!isSessionActive_) return;
    
    std::string status = success ? "completed" : "error";
    std::string sql = "UPDATE processing_sessions SET status = '" + status + "'";
    
    if (!success && !errorMsg.empty()) {
        sql += ", error_message = '" + postgresConnection_.escapeString(errorMsg) + "'";
    }
    
    sql += ", snapshots_written = " + std::to_string(totalSnapshotsWritten_.load());
    sql += ", end_time = CURRENT_TIMESTAMP";
    sql += " WHERE session_id = '" + postgresConnection_.escapeString(activeSessionId_) + "'";
    
    postgresConnection_.execute(sql);
    
    isSessionActive_ = false;
}

void DatabaseWriter::updateSessionStats(size_t messagesReceived, size_t ordersProcessed, 
                                       double throughput, int64_t avgProcessNs, uint64_t p99ProcessNs) {
    if (!isSessionActive_) return;
    
    std::stringstream ss;
    ss << "UPDATE processing_sessions SET "
       << "messages_received = " << messagesReceived << ", "
       << "orders_processed = " << ordersProcessed << ", "
       << "throughput = " << throughput << ", "
       << "avg_process_ns = " << avgProcessNs << ", "
       << "p99_process_ns = " << p99ProcessNs
       << " WHERE session_id = '" << postgresConnection_.escapeString(activeSessionId_) << "'";
    
    postgresConnection_.execute(ss.str());
}

void DatabaseWriter::updateFinalBookState(size_t totalOrders, size_t bidLevels, size_t askLevels,
                                         double bestBid, double bestAsk, double spread) {
    if (!isSessionActive_) return;
    
    std::stringstream ss;
    ss << "UPDATE processing_sessions SET "
       << "final_total_orders = " << totalOrders << ", "
       << "final_bid_levels = " << bidLevels << ", "
       << "final_ask_levels = " << askLevels << ", "
       << "final_best_bid = " << bestBid << ", "
       << "final_best_ask = " << bestAsk << ", "
       << "final_spread = " << spread
       << " WHERE session_id = '" << postgresConnection_.escapeString(activeSessionId_) << "'";
    
    postgresConnection_.execute(ss.str());
}

bool DatabaseWriter::writeBatch(const std::vector<MboMessageWrapper>& batch) {
    if (batch.empty()) {
        return true;
    }
    
    if (!isSessionActive_) {
        return false;
    }
    
    if (!postgresConnection_.isConnected()) {
        return false;
    }
    
    // Use COPY command for maximum performance
    // Start transaction
    if (!postgresConnection_.beginTransaction()) {
        return false;
    }
    
    // Get starting snapshot ID BEFORE starting COPY (for foreign keys)
    auto seqResult = postgresConnection_.execute("SELECT nextval('order_book_snapshots_id_seq') as start_id");
    if (!seqResult.success || seqResult.rows.empty()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    int64_t startSnapshotId = std::stoll(seqResult.rows[0][0]);
    
    // Advance sequence by batch size - 1 (we already got the first one)
    if (batch.size() > 1) {
        std::stringstream advanceSeq;
        advanceSeq << "SELECT setval('order_book_snapshots_id_seq', " 
                   << (startSnapshotId + batch.size() - 1) << ")";
        postgresConnection_.execute(advanceSeq.str());
    }
    
    // COPY snapshots (including id since we're pre-allocating)
    std::vector<std::string> snapshotCols = {
        "id", "session_id", "symbol", "timestamp_ns",
        "best_bid_price", "best_bid_size", "best_bid_count",
        "best_ask_price", "best_ask_size", "best_ask_count",
        "total_orders", "bid_level_count", "ask_level_count"
    };
    
    if (!postgresConnection_.beginCopy("order_book_snapshots", snapshotCols)) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // Build COPY data for snapshots
    std::stringstream snapshotData;
    int64_t currentSnapshotId = startSnapshotId;
    for (const auto& wrapper : batch) {
        const auto& snap = wrapper.snapshot;
        snapshotData << currentSnapshotId << '\t'
                     << activeSessionId_ << '\t'
                     << snap.symbol << '\t'
                     << snap.ts_ns << '\t'
                     << snap.bid.price << '\t' << snap.bid.size << '\t' << snap.bid.count << '\t'
                     << snap.ask.price << '\t' << snap.ask.size << '\t' << snap.ask.count << '\t'
                     << snap.total_orders << '\t' << snap.bid_levels << '\t' << snap.ask_levels << '\n';
        currentSnapshotId++;
    }
    
    if (!postgresConnection_.putCopyData(snapshotData.str()) || !postgresConnection_.endCopy()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // COPY bid levels
    std::vector<std::string> bidCols = {"snapshot_id", "price", "size", "count", "level_index"};
    if (!postgresConnection_.beginCopy("bid_levels", bidCols)) {
        postgresConnection_.rollbackTransaction();
            return false;
    }
    
    std::stringstream bidData;
    currentSnapshotId = startSnapshotId;  // Reset to start
    for (const auto& wrapper : batch) {
        const auto& snap = wrapper.snapshot;
        for (size_t i = 0; i < snap.bids.size(); ++i) {
            const auto& bid = snap.bids[i];
            bidData << currentSnapshotId << '\t' << bid.price << '\t' 
                    << bid.size << '\t' << bid.count << '\t' << i << '\n';
        }
        currentSnapshotId++;
    }
    
    if (!postgresConnection_.putCopyData(bidData.str()) || !postgresConnection_.endCopy()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // COPY ask levels
    std::vector<std::string> askCols = {"snapshot_id", "price", "size", "count", "level_index"};
    if (!postgresConnection_.beginCopy("ask_levels", askCols)) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    std::stringstream askData;
    currentSnapshotId = startSnapshotId;  // Reset to start
    for (const auto& wrapper : batch) {
        const auto& snap = wrapper.snapshot;
        for (size_t i = 0; i < snap.asks.size(); ++i) {
            const auto& ask = snap.asks[i];
            askData << currentSnapshotId << '\t' << ask.price << '\t' 
                    << ask.size << '\t' << ask.count << '\t' << i << '\n';
        }
        currentSnapshotId++;
    }
    
    if (!postgresConnection_.putCopyData(askData.str()) || !postgresConnection_.endCopy()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // Commit transaction
    if (!postgresConnection_.commitTransaction()) {
            return false;
        }
    
    totalSnapshotsWritten_ += batch.size();
    
    // Log progress every 20000 items
    if (totalSnapshotsWritten_ % 20000 == 0) {
        std::cout << "Written " << totalSnapshotsWritten_.load() << " snapshots..." << std::endl;
    }
    
    return true;
}

bool DatabaseWriter::dropIndexes() {
    if (!postgresConnection_.isConnected()) {
        return false;
    }
    
    // Drop non-primary key indexes for faster bulk loading
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_snapshots_symbol_ts");
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_snapshots_session");
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_bid_levels_snapshot_id");
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_ask_levels_snapshot_id");
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_sessions_symbol");
    
    return true;
}

bool DatabaseWriter::recreateIndexes() {
    if (!postgresConnection_.isConnected()) {
        return false;
    }
    
    auto result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_snapshots_symbol_ts ON order_book_snapshots (symbol, timestamp_ns)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_snapshots_symbol_ts");
        return false;
    }
    
    result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_snapshots_session ON order_book_snapshots (session_id, timestamp_ns)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_snapshots_session");
        return false;
    }
    
    result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_bid_levels_snapshot_id ON bid_levels (snapshot_id, level_index)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_bid_levels_snapshot_id");
        return false;
    }
    
    result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_ask_levels_snapshot_id ON ask_levels (snapshot_id, level_index)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_ask_levels_snapshot_id");
        return false;
    }
    
    result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_sessions_symbol ON processing_sessions (symbol, status, start_time DESC)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_sessions_symbol");
        return false;
    }
    
    return true;
}

} // namespace project
