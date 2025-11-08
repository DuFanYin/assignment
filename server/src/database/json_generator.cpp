#include "database/json_generator.hpp"
#include "project/utils.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace project {

JSONGenerator::JSONGenerator(const PostgresConnection::Config& config)
    : conn_(config) {
    if (!conn_.connect()) {
        throw std::runtime_error("Failed to connect to PostgreSQL database");
    }
}

JSONGenerator::~JSONGenerator() {
    conn_.disconnect();
}

std::string JSONGenerator::generateJSON(const std::string& sessionId) {
    if (!conn_.isConnected()) {
        return "{\"error\":\"Not connected to database\"}";
    }
    
    // Query all snapshots for this session
    std::string sql = "SELECT id, symbol, timestamp_ns, best_bid_price, best_bid_size, best_bid_count, "
                     "best_ask_price, best_ask_size, best_ask_count, total_orders, bid_level_count, ask_level_count "
                     "FROM order_book_snapshots WHERE session_id = '" + conn_.escapeString(sessionId) + "' "
                     "ORDER BY timestamp_ns ASC";
    
    auto result = conn_.execute(sql);
    if (!result.success) {
        return "{\"error\":\"Query failed: " + result.errorMessage + "\"}";
    }
    
    return buildJSON(result);
}

std::string JSONGenerator::generateJSONForSymbol(const std::string& symbol) {
    if (!conn_.isConnected()) {
        return "{\"error\":\"Not connected to database\"}";
    }
    
    // Find the latest session for this symbol
    std::string sql = "SELECT session_id FROM processing_sessions "
                     "WHERE symbol = '" + conn_.escapeString(symbol) + "' AND status = 'completed' "
                     "ORDER BY start_time DESC LIMIT 1";
    
    auto result = conn_.execute(sql);
    if (!result.success || result.rows.empty()) {
        return "{\"error\":\"No completed sessions found for symbol\"}";
    }
    
    std::string sessionId = result.rows[0][0];
    
    return generateJSON(sessionId);
}

std::string JSONGenerator::fetchLevels(const std::string& snapshotId, const std::string& tableName) {
    std::string sql = "SELECT price, size, count FROM " + tableName + 
                     " WHERE snapshot_id = " + snapshotId + " ORDER BY level_index ASC";
    
    auto result = conn_.execute(sql);
    if (!result.success) {
        return "[]";
    }
    
    nlohmann::json levels = nlohmann::json::array();
    
    for (const auto& row : result.rows) {
        if (row.size() >= 3) {
            nlohmann::json level;
            level["price"] = row[0];
            level["size"] = std::atoi(row[1].c_str());
            level["count"] = std::atoi(row[2].c_str());
            levels.push_back(level);
        }
    }
    
    return levels.dump();
}

std::string JSONGenerator::buildJSON(const PostgresConnection::QueryResult& snapshotsRes) {
    if (snapshotsRes.rows.empty()) {
        return "{\"error\":\"No snapshots found\"}";
    }
    
    // Build newline-delimited JSON output (same format as original)
    std::stringstream output;
    
    for (const auto& row : snapshotsRes.rows) {
        if (row.size() < 12) continue;
        
        nlohmann::json json;
        
        std::string snapshotId = row[0];
        std::string symbol = row[1];
        std::string timestampNs = row[2];
        std::string bestBidPrice = row[3];
        std::string bestBidSize = row[4];
        std::string bestBidCount = row[5];
        std::string bestAskPrice = row[6];
        std::string bestAskSize = row[7];
        std::string bestAskCount = row[8];
        std::string totalOrders = row[9];
        std::string bidLevelCount = row[10];
        std::string askLevelCount = row[11];
        
        json["symbol"] = symbol;
        json["timestamp"] = timestampNs;
        json["timestamp_ns"] = std::stoll(timestampNs);
        
        // BBO
        nlohmann::json bbo;
        if (!bestBidPrice.empty() && bestBidPrice != "-9223372036854775808") {
            nlohmann::json bid;
            bid["price"] = bestBidPrice;
            bid["size"] = std::atoi(bestBidSize.c_str());
            bid["count"] = std::atoi(bestBidCount.c_str());
            bbo["bid"] = bid;
        } else {
            bbo["bid"] = nullptr;
        }
        
        if (!bestAskPrice.empty() && bestAskPrice != "-9223372036854775808") {
            nlohmann::json ask;
            ask["price"] = bestAskPrice;
            ask["size"] = std::atoi(bestAskSize.c_str());
            ask["count"] = std::atoi(bestAskCount.c_str());
            bbo["ask"] = ask;
        } else {
            bbo["ask"] = nullptr;
        }
        json["bbo"] = bbo;
        
        // Levels
        nlohmann::json levels;
        std::string bidsJson = fetchLevels(snapshotId, "bid_levels");
        std::string asksJson = fetchLevels(snapshotId, "ask_levels");
        
        levels["bids"] = nlohmann::json::parse(bidsJson);
        levels["asks"] = nlohmann::json::parse(asksJson);
        json["levels"] = levels;
        
        // Stats
        nlohmann::json stats;
        stats["total_orders"] = std::stoull(totalOrders);
        stats["bid_levels"] = std::stoull(bidLevelCount);
        stats["ask_levels"] = std::stoull(askLevelCount);
        json["stats"] = stats;
        
        output << json.dump() << "\n";
    }
    
    return output.str();
}

} // namespace project
