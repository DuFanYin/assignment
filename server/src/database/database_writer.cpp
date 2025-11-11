#include "database/database_writer.hpp"
#include "project/utils.hpp"
#include "project/server.hpp"  // For MboMessageWrapper
#include <sstream>
#include <chrono>
#include <random>

namespace project {

DatabaseWriter::DatabaseWriter(const PostgresConnection::Config& config)
    : postgresConnection_(config) {
    if (!postgresConnection_.connect()) {
        throw std::runtime_error("Failed to connect to PostgreSQL database");
    }
}

DatabaseWriter::~DatabaseWriter() {
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

void DatabaseWriter::startSession(const std::string& symbol, const std::string& fileName, size_t fileSize) {
    activeSessionId_ = generateSessionId();
    
    // Aggressive performance tuning for bulk load
    postgresConnection_.execute("SET synchronous_commit = off");
    postgresConnection_.execute("SET wal_writer_delay = '200ms'");  // Batch WAL writes
    postgresConnection_.execute("SET commit_delay = 100000");       // 100ms commit delay
    postgresConnection_.execute("SET commit_siblings = 1");         // Enable commit delay
    postgresConnection_.execute("SET maintenance_work_mem = '256MB'"); // For index creation
    
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
    
    // Preallocate snapshot IDs upfront (much faster than querying after)
    auto seqResult = postgresConnection_.execute(
        "SELECT nextval('order_book_snapshots_id_seq') FROM generate_series(1, " + 
        std::to_string(batch.size()) + ")"
    );
    if (!seqResult.success || seqResult.rows.size() != batch.size()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    std::vector<int64_t> snapshotIds;
    snapshotIds.reserve(batch.size());
    for (const auto& row : seqResult.rows) {
        snapshotIds.push_back(std::stoll(row[0]));
    }
    
    // COPY snapshots (with explicit IDs and JSONB levels)
    std::vector<std::string> snapshotCols = {
        "id", "session_id", "symbol", "timestamp_ns",
        "best_bid_price", "best_bid_size", "best_bid_count",
        "best_ask_price", "best_ask_size", "best_ask_count",
        "total_orders", "bid_level_count", "ask_level_count",
        "bid_levels_json", "ask_levels_json"
    };
    
    if (!postgresConnection_.beginCopyBinary("order_book_snapshots", snapshotCols)) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // Helpers to build COPY BINARY buffers
    auto append_be16 = [](std::string& buf, uint16_t v) {
        char bytes[2];
        bytes[0] = static_cast<char>((v >> 8) & 0xFF);
        bytes[1] = static_cast<char>(v & 0xFF);
        buf.append(bytes, 2);
    };
    auto append_be32 = [](std::string& buf, uint32_t v) {
        char bytes[4];
        bytes[0] = static_cast<char>((v >> 24) & 0xFF);
        bytes[1] = static_cast<char>((v >> 16) & 0xFF);
        bytes[2] = static_cast<char>((v >> 8) & 0xFF);
        bytes[3] = static_cast<char>(v & 0xFF);
        buf.append(bytes, 4);
    };
    auto append_be64 = [](std::string& buf, uint64_t v) {
        char bytes[8];
        bytes[0] = static_cast<char>((v >> 56) & 0xFF);
        bytes[1] = static_cast<char>((v >> 48) & 0xFF);
        bytes[2] = static_cast<char>((v >> 40) & 0xFF);
        bytes[3] = static_cast<char>((v >> 32) & 0xFF);
        bytes[4] = static_cast<char>((v >> 24) & 0xFF);
        bytes[5] = static_cast<char>((v >> 16) & 0xFF);
        bytes[6] = static_cast<char>((v >> 8) & 0xFF);
        bytes[7] = static_cast<char>(v & 0xFF);
        buf.append(bytes, 8);
    };
    auto append = [](std::string& buf, const void* p, size_t n) {
        buf.append(reinterpret_cast<const char*>(p), n);
    };
    auto append_i16 = [&](std::string& buf, int16_t v) {
        append_be16(buf, static_cast<uint16_t>(v));
    };
    auto append_i32 = [&](std::string& buf, int32_t v) {
        append_be32(buf, static_cast<uint32_t>(v));
    };
    auto append_i64 = [&](std::string& buf, int64_t v) {
        append_be64(buf, static_cast<uint64_t>(v));
    };
    auto append_text_field = [&](std::string& buf, const std::string& s) {
        append_i32(buf, static_cast<int32_t>(s.size()));
        append(buf, s.data(), s.size());
    };
    auto append_int4_field = [&](std::string& buf, int32_t v) {
        append_i32(buf, 4);
        append_i32(buf, v);
    };
    auto append_int8_field = [&](std::string& buf, int64_t v) {
        append_i32(buf, 8);
        append_i64(buf, v);
    };
    
    // Build COPY BINARY header
    auto build_copy_binary_header = [&]() {
        std::string hdr;
        static const char sig[] = "PGCOPY\n\377\r\n\0";
        hdr.append(sig, sizeof(sig) - 1);
        append_i32(hdr, 0); // flags
        append_i32(hdr, 0); // header extension length
        return hdr;
    };
    auto build_copy_binary_trailer = [&]() {
        std::string tr;
        append_i16(tr, static_cast<int16_t>(-1));
        return tr;
    };
    
    std::string snapshotData;
    snapshotData.reserve(batch.size() * 1024); // Increased reservation for JSONB
    snapshotData += build_copy_binary_header();
    
    // Pre-allocate reusable buffers for JSON building (avoid repeated allocations)
    std::string bidJsonStr;
    std::string askJsonStr;
    bidJsonStr.reserve(512);  // Reserve space for typical JSON size
    askJsonStr.reserve(512);
    
    // Fast integer-to-string conversion buffer (reusable)
    char intBuf[32];
    
    // Helper for fast integer append (avoids std::to_string allocation)
    auto append_int64 = [&intBuf](std::string& str, int64_t value) {
        int len = snprintf(intBuf, sizeof(intBuf), "%lld", static_cast<long long>(value));
        str.append(intBuf, len);
    };
    auto append_uint32 = [&intBuf](std::string& str, uint32_t value) {
        int len = snprintf(intBuf, sizeof(intBuf), "%u", value);
        str.append(intBuf, len);
    };
    
    for (size_t batchIdx = 0; batchIdx < batch.size(); ++batchIdx) {
        const auto& snap = batch[batchIdx].snapshot;
        int64_t snapId = snapshotIds[batchIdx];
        
        // Build JSONB for bid levels with pre-allocated buffer and fast int conversion
        bidJsonStr.clear();
        bidJsonStr.push_back('[');
        for (size_t i = 0; i < snap.bids.size(); ++i) {
            if (i > 0) bidJsonStr.push_back(',');
            bidJsonStr.append("{\"price\":");
            append_int64(bidJsonStr, snap.bids[i].price);
            bidJsonStr.append(",\"size\":");
            append_uint32(bidJsonStr, snap.bids[i].size);
            bidJsonStr.append(",\"count\":");
            append_uint32(bidJsonStr, snap.bids[i].count);
            bidJsonStr.push_back('}');
        }
        bidJsonStr.push_back(']');
        
        // Build JSONB for ask levels with pre-allocated buffer and fast int conversion
        askJsonStr.clear();
        askJsonStr.push_back('[');
        for (size_t i = 0; i < snap.asks.size(); ++i) {
            if (i > 0) askJsonStr.push_back(',');
            askJsonStr.append("{\"price\":");
            append_int64(askJsonStr, snap.asks[i].price);
            askJsonStr.append(",\"size\":");
            append_uint32(askJsonStr, snap.asks[i].size);
            askJsonStr.append(",\"count\":");
            append_uint32(askJsonStr, snap.asks[i].count);
            askJsonStr.push_back('}');
        }
        askJsonStr.push_back(']');
        
        // number of fields (15 now with JSONB)
        append_i16(snapshotData, static_cast<int16_t>(15));
        // id BIGINT
        append_int8_field(snapshotData, snapId);
        // session_id TEXT
        append_text_field(snapshotData, activeSessionId_);
        // symbol TEXT
        append_text_field(snapshotData, snap.symbol);
        // timestamp_ns BIGINT
        append_int8_field(snapshotData, static_cast<int64_t>(snap.ts_ns));
        // best_bid_price BIGINT
        append_int8_field(snapshotData, static_cast<int64_t>(snap.bid.price));
        // best_bid_size INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.bid.size));
        // best_bid_count INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.bid.count));
        // best_ask_price BIGINT
        append_int8_field(snapshotData, static_cast<int64_t>(snap.ask.price));
        // best_ask_size INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.ask.size));
        // best_ask_count INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.ask.count));
        // total_orders BIGINT
        append_int8_field(snapshotData, static_cast<int64_t>(snap.total_orders));
        // bid_level_count INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.bid_levels));
        // ask_level_count INT
        append_int4_field(snapshotData, static_cast<int32_t>(snap.ask_levels));
        // bid_levels_json JSONB (binary format: version byte + JSON text)
        append_i32(snapshotData, static_cast<int32_t>(bidJsonStr.size() + 1)); // +1 for version byte
        snapshotData.push_back(0x01); // JSONB version 1
        snapshotData.append(bidJsonStr);
        
        // ask_levels_json JSONB (binary format: version byte + JSON text)
        append_i32(snapshotData, static_cast<int32_t>(askJsonStr.size() + 1)); // +1 for version byte
        snapshotData.push_back(0x01); // JSONB version 1
        snapshotData.append(askJsonStr);
    }
    snapshotData += build_copy_binary_trailer();
    
    if (!postgresConnection_.putCopyData(snapshotData)) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    if (!postgresConnection_.endCopy()) {
        postgresConnection_.rollbackTransaction();
        return false;
    }
    
    // Commit transaction (no separate bid/ask tables needed!)
    // Note: With synchronous_commit=off and commit_delay, this returns immediately
    if (!postgresConnection_.commitTransaction()) {
        return false;
    }
    
    totalSnapshotsWritten_ += batch.size();
    
    return true;
}

bool DatabaseWriter::dropIndexes() {
    if (!postgresConnection_.isConnected()) {
        return false;
    }
    
    // Drop non-primary key indexes for faster bulk loading
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_snapshots_symbol_ts");
    postgresConnection_.execute("DROP INDEX IF EXISTS idx_snapshots_session");
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
    
    result = postgresConnection_.execute("CREATE INDEX IF NOT EXISTS idx_sessions_symbol ON processing_sessions (symbol, status, start_time DESC)");
    if (!result.success) {
        utils::logError("Failed to recreate idx_sessions_symbol");
        return false;
    }
    
    return true;
}

} // namespace project
