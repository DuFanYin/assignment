#include "database/database_writer.hpp"
#include "project/utils.hpp"
#include "project/server.hpp"  // For MboMessageWrapper
#include <clickhouse/client.h>
#include <clickhouse/columns/column.h>
#include <clickhouse/columns/array.h>
#include <clickhouse/columns/tuple.h>
#include <sstream>
#include <chrono>
#include <random>

namespace project {

DatabaseWriter::DatabaseWriter(const ClickHouseConnection::Config& config)
    : clickhouseConnection_(config) {
    if (!clickhouseConnection_.connect()) {
        std::stringstream ss;
        ss << "Failed to connect to ClickHouse database";
        const std::string& detail = clickhouseConnection_.getLastError();
        if (!detail.empty()) {
            ss << ": " << detail;
        }
        throw std::runtime_error(ss.str());
    }
}

DatabaseWriter::~DatabaseWriter() {
    clickhouseConnection_.disconnect();
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

void DatabaseWriter::startSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    activeSessionId_ = generateSessionId();
    
    // ClickHouse doesn't need aggressive tuning - it's fast by default!
    // No WAL, no synchronous commits, columnar storage = naturally fast
    
    if (!insertSession(symbol, fileName, fileSize)) {
        throw std::runtime_error("Failed to create database session");
    }
    
    isSessionActive_ = true;
    totalSnapshotsWritten_ = 0;
    nextId_ = 1;
}

bool DatabaseWriter::insertSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    try {
        auto client = clickhouseConnection_.getClient();
        if (!client) return false;
        
        // Create block with session data
        clickhouse::Block block;
        
        auto session_id = std::make_shared<clickhouse::ColumnString>();
        auto symbol_col = std::make_shared<clickhouse::ColumnString>();
        auto file_name = std::make_shared<clickhouse::ColumnString>();
        auto file_size_col = std::make_shared<clickhouse::ColumnUInt64>();
        auto status = std::make_shared<clickhouse::ColumnString>();
        
        session_id->Append(activeSessionId_);
        symbol_col->Append(symbol);
        file_name->Append(fileName);
        file_size_col->Append(fileSize);
        status->Append("processing");
        
        block.AppendColumn("session_id", session_id);
        block.AppendColumn("symbol", symbol_col);
        block.AppendColumn("file_name", file_name);
        block.AppendColumn("file_size", file_size_col);
        block.AppendColumn("status", status);
        
        client->Insert("processing_sessions", block);
        return true;
    } catch (const std::exception& e) {
        utils::logError("Failed to insert session: " + std::string(e.what()));
        return false;
    }
}

void DatabaseWriter::endSession(bool success, const std::string& errorMsg) {
    if (!isSessionActive_) return;
    
    try {
        auto client = clickhouseConnection_.getClient();
        if (!client) return;
        
        std::string status = success ? "completed" : "error";
        std::stringstream query;
        query << "ALTER TABLE processing_sessions UPDATE "
              << "status = '" << status << "', "
              << "snapshots_written = " << totalSnapshotsWritten_.load() << ", "
              << "end_time = now()";
        
        if (!success && !errorMsg.empty()) {
            query << ", error_message = '" << errorMsg << "'";
        }
        
        query << " WHERE session_id = '" << activeSessionId_ << "'";
        
        clickhouseConnection_.execute(query.str());
    } catch (const std::exception& e) {
        utils::logError("Failed to end session: " + std::string(e.what()));
    }
    
    isSessionActive_ = false;
}

void DatabaseWriter::updateSessionStats(size_t messagesReceived, size_t ordersProcessed, 
                                       double throughput, int64_t avgProcessNs, uint64_t p99ProcessNs) {
    if (!isSessionActive_) return;
    
    try {
        std::stringstream query;
        query << "ALTER TABLE processing_sessions UPDATE "
              << "messages_received = " << messagesReceived << ", "
              << "orders_processed = " << ordersProcessed << ", "
              << "throughput = " << throughput << ", "
              << "avg_process_ns = " << avgProcessNs << ", "
              << "p99_process_ns = " << p99ProcessNs
              << " WHERE session_id = '" << activeSessionId_ << "'";
        
        clickhouseConnection_.execute(query.str());
    } catch (const std::exception& e) {
        utils::logError("Failed to update session stats: " + std::string(e.what()));
    }
}

void DatabaseWriter::updateFinalBookState(size_t totalOrders, size_t bidLevels, size_t askLevels,
                                         double bestBid, double bestAsk, double spread) {
    if (!isSessionActive_) return;
    
    try {
        std::stringstream query;
        query << "ALTER TABLE processing_sessions UPDATE "
              << "final_total_orders = " << totalOrders << ", "
              << "final_bid_levels = " << bidLevels << ", "
              << "final_ask_levels = " << askLevels << ", "
              << "final_best_bid = " << bestBid << ", "
              << "final_best_ask = " << bestAsk << ", "
              << "final_spread = " << spread
              << " WHERE session_id = '" << activeSessionId_ << "'";
        
        clickhouseConnection_.execute(query.str());
    } catch (const std::exception& e) {
        utils::logError("Failed to update final book state: " + std::string(e.what()));
    }
}

bool DatabaseWriter::writeBatch(const std::vector<MboMessageWrapper>& batch) {
    if (batch.empty()) {
        return true;
    }
    
    if (!isSessionActive_) {
        return false;
    }
    
    try {
        auto client = clickhouseConnection_.getClient();
        if (!client) return false;
        
        // Create ClickHouse Block with all columns
        clickhouse::Block block;
        
        // Create all scalar columns
        auto id_col = std::make_shared<clickhouse::ColumnUInt64>();
        auto session_col = std::make_shared<clickhouse::ColumnString>();
        auto symbol_col = std::make_shared<clickhouse::ColumnString>();
        auto timestamp_col = std::make_shared<clickhouse::ColumnInt64>();
        auto best_bid_price = std::make_shared<clickhouse::ColumnInt64>();
        auto best_bid_size = std::make_shared<clickhouse::ColumnUInt32>();
        auto best_bid_count = std::make_shared<clickhouse::ColumnUInt32>();
        auto best_ask_price = std::make_shared<clickhouse::ColumnInt64>();
        auto best_ask_size = std::make_shared<clickhouse::ColumnUInt32>();
        auto best_ask_count = std::make_shared<clickhouse::ColumnUInt32>();
        auto total_orders = std::make_shared<clickhouse::ColumnUInt64>();
        auto bid_level_count = std::make_shared<clickhouse::ColumnUInt32>();
        auto ask_level_count = std::make_shared<clickhouse::ColumnUInt32>();
        
        // Create tuple columns for array elements
        // bid_levels: Array(Tuple(price Int64, size UInt32, count UInt32))
        auto bid_price_col = std::make_shared<clickhouse::ColumnInt64>();
        auto bid_size_col = std::make_shared<clickhouse::ColumnUInt32>();
        auto bid_count_col = std::make_shared<clickhouse::ColumnUInt32>();
        auto bid_tuple = std::make_shared<clickhouse::ColumnTuple>(
            std::vector<clickhouse::ColumnRef>{bid_price_col, bid_size_col, bid_count_col}
        );
        auto bid_levels = std::make_shared<clickhouse::ColumnArray>(bid_tuple);
        
        // ask_levels: Array(Tuple(price Int64, size UInt32, count UInt32))
        auto ask_price_col = std::make_shared<clickhouse::ColumnInt64>();
        auto ask_size_col = std::make_shared<clickhouse::ColumnUInt32>();
        auto ask_count_col = std::make_shared<clickhouse::ColumnUInt32>();
        auto ask_tuple = std::make_shared<clickhouse::ColumnTuple>(
            std::vector<clickhouse::ColumnRef>{ask_price_col, ask_size_col, ask_count_col}
        );
        auto ask_levels = std::make_shared<clickhouse::ColumnArray>(ask_tuple);
        
        // Fill columns for each snapshot in batch
        for (const auto& wrapper : batch) {
            const auto& snap = wrapper.snapshot;
            
            // Scalar fields
            id_col->Append(nextId_++);
            session_col->Append(activeSessionId_);
            symbol_col->Append(snap.symbol);
            timestamp_col->Append(static_cast<int64_t>(snap.ts_ns));
            best_bid_price->Append(static_cast<int64_t>(snap.bid.price));
            best_bid_size->Append(static_cast<uint32_t>(snap.bid.size));
            best_bid_count->Append(static_cast<uint32_t>(snap.bid.count));
            best_ask_price->Append(static_cast<int64_t>(snap.ask.price));
            best_ask_size->Append(static_cast<uint32_t>(snap.ask.size));
            best_ask_count->Append(static_cast<uint32_t>(snap.ask.count));
            total_orders->Append(snap.total_orders);
            bid_level_count->Append(static_cast<uint32_t>(snap.bid_levels));
            ask_level_count->Append(static_cast<uint32_t>(snap.ask_levels));
            
            // Build bid_levels array for this row
            auto bid_array_price = std::make_shared<clickhouse::ColumnInt64>();
            auto bid_array_size = std::make_shared<clickhouse::ColumnUInt32>();
            auto bid_array_count = std::make_shared<clickhouse::ColumnUInt32>();
            
            for (const auto& level : snap.bids) {
                bid_array_price->Append(static_cast<int64_t>(level.price));
                bid_array_size->Append(static_cast<uint32_t>(level.size));
                bid_array_count->Append(static_cast<uint32_t>(level.count));
            }
            
            auto bid_row_tuple = std::make_shared<clickhouse::ColumnTuple>(
                std::vector<clickhouse::ColumnRef>{bid_array_price, bid_array_size, bid_array_count}
            );
            bid_levels->AppendAsColumn(bid_row_tuple);
            
            // Build ask_levels array for this row
            auto ask_array_price = std::make_shared<clickhouse::ColumnInt64>();
            auto ask_array_size = std::make_shared<clickhouse::ColumnUInt32>();
            auto ask_array_count = std::make_shared<clickhouse::ColumnUInt32>();
            
            for (const auto& level : snap.asks) {
                ask_array_price->Append(static_cast<int64_t>(level.price));
                ask_array_size->Append(static_cast<uint32_t>(level.size));
                ask_array_count->Append(static_cast<uint32_t>(level.count));
            }
            
            auto ask_row_tuple = std::make_shared<clickhouse::ColumnTuple>(
                std::vector<clickhouse::ColumnRef>{ask_array_price, ask_array_size, ask_array_count}
            );
            ask_levels->AppendAsColumn(ask_row_tuple);
        }
        
        // Append all columns to block
        block.AppendColumn("id", id_col);
        block.AppendColumn("session_id", session_col);
        block.AppendColumn("symbol", symbol_col);
        block.AppendColumn("timestamp_ns", timestamp_col);
        block.AppendColumn("best_bid_price", best_bid_price);
        block.AppendColumn("best_bid_size", best_bid_size);
        block.AppendColumn("best_bid_count", best_bid_count);
        block.AppendColumn("best_ask_price", best_ask_price);
        block.AppendColumn("best_ask_size", best_ask_size);
        block.AppendColumn("best_ask_count", best_ask_count);
        block.AppendColumn("total_orders", total_orders);
        block.AppendColumn("bid_level_count", bid_level_count);
        block.AppendColumn("ask_level_count", ask_level_count);
        block.AppendColumn("bid_levels", bid_levels);
        block.AppendColumn("ask_levels", ask_levels);
        
        // Insert block into ClickHouse (super fast!)
        client->Insert("order_book_snapshots", block);
        
        totalSnapshotsWritten_ += batch.size();
        return true;
        
    } catch (const std::exception& e) {
        utils::logError("Failed to write batch to ClickHouse: " + std::string(e.what()));
        return false;
    }
}

} // namespace project
