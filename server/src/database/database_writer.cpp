#include "database/database_writer.hpp"
#include "project/utils.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <ctime>

namespace project {

DatabaseWriter::DatabaseWriter(const PostgresConnection::Config& config)
    : conn_(config) {
    if (!conn_.connect()) {
        throw std::runtime_error("Failed to connect to PostgreSQL database");
    }
    
    if (!prepareStatements()) {
        throw std::runtime_error("Failed to prepare SQL statements");
    }
}

DatabaseWriter::~DatabaseWriter() {
    clearPreparedStatements();
    conn_.disconnect();
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
    // Prepare INSERT snapshot statement
    const std::string insertSnapshotSQL = 
        "INSERT INTO order_book_snapshots "
        "(session_id, symbol, timestamp_ns, best_bid_price, best_bid_size, best_bid_count, "
        " best_ask_price, best_ask_size, best_ask_count, total_orders, bid_level_count, ask_level_count) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) "
        "RETURNING id";
    
    if (!conn_.prepareStatement("insert_snapshot", insertSnapshotSQL)) {
        utils::logError("Failed to prepare insert_snapshot: " + conn_.getLastError());
        return false;
    }
    
    // Prepare INSERT bid level statement
    const std::string insertBidSQL = 
        "INSERT INTO bid_levels (snapshot_id, price, size, count, level_index) "
        "VALUES ($1, $2, $3, $4, $5)";
    
    if (!conn_.prepareStatement("insert_bid_level", insertBidSQL)) {
        utils::logError("Failed to prepare insert_bid_level: " + conn_.getLastError());
        return false;
    }
    
    // Prepare INSERT ask level statement
    const std::string insertAskSQL = 
        "INSERT INTO ask_levels (snapshot_id, price, size, count, level_index) "
        "VALUES ($1, $2, $3, $4, $5)";
    
    if (!conn_.prepareStatement("insert_ask_level", insertAskSQL)) {
        utils::logError("Failed to prepare insert_ask_level: " + conn_.getLastError());
        return false;
    }
    
    return true;
}

void DatabaseWriter::clearPreparedStatements() {
    if (!conn_.isConnected()) return;
    
    conn_.execute("DEALLOCATE insert_snapshot");
    conn_.execute("DEALLOCATE insert_bid_level");
    conn_.execute("DEALLOCATE insert_ask_level");
}

void DatabaseWriter::startSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    currentSessionId_ = generateSessionId();
    
    if (!insertSession(symbol, fileName, fileSize)) {
        throw std::runtime_error("Failed to create database session");
    }
    
    sessionActive_ = true;
    snapshotsWritten_ = 0;
    
    utils::logInfo("Started database session: " + currentSessionId_);
}

bool DatabaseWriter::insertSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    std::string sql = "INSERT INTO processing_sessions "
                     "(session_id, symbol, file_name, file_size, status) "
                     "VALUES ('" + conn_.escapeString(currentSessionId_) + "', '" + 
                     conn_.escapeString(symbol) + "', '" + 
                     conn_.escapeString(fileName) + "', " + std::to_string(fileSize) + ", 'processing')";
    
    auto result = conn_.execute(sql);
    return result.success;
}

void DatabaseWriter::endSession(bool success, const std::string& errorMsg) {
    if (!sessionActive_) return;
    
    std::string status = success ? "completed" : "error";
    std::string sql = "UPDATE processing_sessions SET status = '" + status + "'";
    
    if (!success && !errorMsg.empty()) {
        sql += ", error_message = '" + conn_.escapeString(errorMsg) + "'";
    }
    
    sql += ", snapshots_written = " + std::to_string(snapshotsWritten_.load());
    sql += ", end_time = CURRENT_TIMESTAMP";
    sql += " WHERE session_id = '" + conn_.escapeString(currentSessionId_) + "'";
    
    conn_.execute(sql);
    
    sessionActive_ = false;
    utils::logInfo("Ended database session: " + currentSessionId_ + " (status: " + status + ")");
}

void DatabaseWriter::updateSessionStats(size_t messagesReceived, size_t ordersProcessed, 
                                       double throughput, int64_t avgProcessNs, uint64_t p99ProcessNs) {
    if (!sessionActive_) return;
    
    std::stringstream ss;
    ss << "UPDATE processing_sessions SET "
       << "messages_received = " << messagesReceived << ", "
       << "orders_processed = " << ordersProcessed << ", "
       << "throughput = " << throughput << ", "
       << "avg_process_ns = " << avgProcessNs << ", "
       << "p99_process_ns = " << p99ProcessNs
       << " WHERE session_id = '" << conn_.escapeString(currentSessionId_) << "'";
    
    conn_.execute(ss.str());
}

void DatabaseWriter::updateFinalBookState(size_t totalOrders, size_t bidLevels, size_t askLevels,
                                         double bestBid, double bestAsk, double spread) {
    if (!sessionActive_) return;
    
    std::stringstream ss;
    ss << "UPDATE processing_sessions SET "
       << "final_total_orders = " << totalOrders << ", "
       << "final_bid_levels = " << bidLevels << ", "
       << "final_ask_levels = " << askLevels << ", "
       << "final_best_bid = " << bestBid << ", "
       << "final_best_ask = " << bestAsk << ", "
       << "final_spread = " << spread
       << " WHERE session_id = '" << conn_.escapeString(currentSessionId_) << "'";
    
    conn_.execute(ss.str());
}

bool DatabaseWriter::writeSnapshot(const BookSnapshot& snapshot) {
    if (!sessionActive_ || !conn_.isConnected()) {
        return false;
    }
    
    // Prepare parameters for snapshot insertion
    std::vector<std::string> params;
    params.push_back(currentSessionId_);
    params.push_back(snapshot.symbol);
    params.push_back(std::to_string(snapshot.ts_ns));
    params.push_back(std::to_string(snapshot.bid.price));
    params.push_back(std::to_string(snapshot.bid.size));
    params.push_back(std::to_string(snapshot.bid.count));
    params.push_back(std::to_string(snapshot.ask.price));
    params.push_back(std::to_string(snapshot.ask.size));
    params.push_back(std::to_string(snapshot.ask.count));
    params.push_back(std::to_string(snapshot.total_orders));
    params.push_back(std::to_string(snapshot.bid_levels));
    params.push_back(std::to_string(snapshot.ask_levels));
    
    // Execute prepared statement
    auto result = conn_.executePrepared("insert_snapshot", params);
    
    if (!result.success || result.rows.empty()) {
        utils::logError("Failed to insert snapshot: " + result.errorMessage);
        return false;
    }
    
    // Get the snapshot_id from the RETURNING clause
    std::string snapshotId = result.rows[0][0];
    
    // Insert bid levels
    for (size_t i = 0; i < snapshot.bids.size(); ++i) {
        const auto& bid = snapshot.bids[i];
        std::vector<std::string> bidParams;
        bidParams.push_back(snapshotId);
        bidParams.push_back(std::to_string(bid.price));
        bidParams.push_back(std::to_string(bid.size));
        bidParams.push_back(std::to_string(bid.count));
        bidParams.push_back(std::to_string(i));
        
        auto bidResult = conn_.executePrepared("insert_bid_level", bidParams);
        if (!bidResult.success) {
            utils::logError("Failed to insert bid level: " + bidResult.errorMessage);
            return false;
        }
    }
    
    // Insert ask levels
    for (size_t i = 0; i < snapshot.asks.size(); ++i) {
        const auto& ask = snapshot.asks[i];
        std::vector<std::string> askParams;
        askParams.push_back(snapshotId);
        askParams.push_back(std::to_string(ask.price));
        askParams.push_back(std::to_string(ask.size));
        askParams.push_back(std::to_string(ask.count));
        askParams.push_back(std::to_string(i));
        
        auto askResult = conn_.executePrepared("insert_ask_level", askParams);
        if (!askResult.success) {
            utils::logError("Failed to insert ask level: " + askResult.errorMessage);
            return false;
        }
    }
    
    snapshotsWritten_++;
    return true;
}

} // namespace project
