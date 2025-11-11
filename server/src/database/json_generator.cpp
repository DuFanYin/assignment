#include "database/json_generator.hpp"
#include "project/utils.hpp"
#include <nlohmann/json.hpp>
#include <clickhouse/client.h>
#include <clickhouse/columns/column.h>
#include <sstream>
#include <vector>

namespace project {

JSONGenerator::JSONGenerator(const ClickHouseConnection::Config& config)
    : clickhouseConnection_(config) {
    if (!clickhouseConnection_.connect()) {
        throw std::runtime_error("Failed to connect to ClickHouse database");
    }
}

JSONGenerator::~JSONGenerator() {
    clickhouseConnection_.disconnect();
}

std::string JSONGenerator::generateJSON(const std::string& sessionId) {
    if (!clickhouseConnection_.isConnected()) {
        return "{\"error\":\"Not connected to database\"}";
    }
    
    return buildJSON(sessionId);
}

std::string JSONGenerator::generateJSONForSymbol(const std::string& symbol) {
    if (!clickhouseConnection_.isConnected()) {
        return "{\"error\":\"Not connected to database\"}";
    }
    
    try {
        auto client = clickhouseConnection_.getClient();
        if (!client) {
            return "{\"error\":\"ClickHouse client not available\"}";
        }
        
        // Find the latest session for this symbol
        std::string query = "SELECT session_id FROM processing_sessions "
                          "WHERE symbol = '" + symbol + "' AND status = 'completed' "
                          "ORDER BY start_time DESC LIMIT 1";
        
        std::string sessionId;
        client->Select(query, [&sessionId](const clickhouse::Block& block) {
            if (block.GetRowCount() > 0) {
                auto session_col = block[0]->As<clickhouse::ColumnString>();
                sessionId = session_col->At(0);
            }
        });
        
        if (sessionId.empty()) {
            return "{\"error\":\"No completed session found for symbol: " + symbol + "\"}";
        }
        
        return buildJSON(sessionId);
    } catch (const std::exception& e) {
        return "{\"error\":\"Query failed: " + std::string(e.what()) + "\"}";
    }
}

std::string JSONGenerator::buildJSON(const std::string& sessionId) {
    try {
        auto client = clickhouseConnection_.getClient();
        if (!client) {
            return "{\"error\":\"ClickHouse client not available\"}";
        }
        
        // Query all snapshots for this session with bid/ask levels
        std::string query = "SELECT symbol, timestamp_ns, best_bid_price, best_bid_size, best_bid_count, "
                          "best_ask_price, best_ask_size, best_ask_count, total_orders, "
                          "bid_level_count, ask_level_count, bid_levels, ask_levels "
                          "FROM order_book_snapshots WHERE session_id = '" + sessionId + "' "
                          "ORDER BY timestamp_ns ASC";
        
        std::stringstream jsonOutput;
        bool firstRow = true;
        
        client->Select(query, [&jsonOutput, &firstRow](const clickhouse::Block& block) {
            if (block.GetRowCount() == 0) return;
            
            // Extract columns
            auto symbol_col = block[0]->As<clickhouse::ColumnString>();
            auto timestamp_col = block[1]->As<clickhouse::ColumnInt64>();
            auto best_bid_price_col = block[2]->As<clickhouse::ColumnInt64>();
            auto best_bid_size_col = block[3]->As<clickhouse::ColumnUInt32>();
            auto best_bid_count_col = block[4]->As<clickhouse::ColumnUInt32>();
            auto best_ask_price_col = block[5]->As<clickhouse::ColumnInt64>();
            auto best_ask_size_col = block[6]->As<clickhouse::ColumnUInt32>();
            auto best_ask_count_col = block[7]->As<clickhouse::ColumnUInt32>();
            auto total_orders_col = block[8]->As<clickhouse::ColumnUInt64>();
            auto bid_level_count_col = block[9]->As<clickhouse::ColumnUInt32>();
            auto ask_level_count_col = block[10]->As<clickhouse::ColumnUInt32>();
            auto bid_levels_col = block[11]->As<clickhouse::ColumnArray>();
            auto ask_levels_col = block[12]->As<clickhouse::ColumnArray>();
            
            // Process each row
            for (size_t i = 0; i < block.GetRowCount(); ++i) {
                if (!firstRow) {
                    jsonOutput << "\n";
                }
                firstRow = false;
                
                nlohmann::json record;
                record["symbol"] = symbol_col->At(i);
                record["timestamp"] = std::to_string(timestamp_col->At(i));
                record["timestamp_ns"] = timestamp_col->At(i);
                
                // BBO
                nlohmann::json bbo;
                nlohmann::json bid;
                bid["price"] = std::to_string(best_bid_price_col->At(i));
                bid["size"] = best_bid_size_col->At(i);
                bid["count"] = best_bid_count_col->At(i);
                
                nlohmann::json ask;
                ask["price"] = std::to_string(best_ask_price_col->At(i));
                ask["size"] = best_ask_size_col->At(i);
                ask["count"] = best_ask_count_col->At(i);
                
                bbo["bid"] = bid;
                bbo["ask"] = ask;
                record["bbo"] = bbo;
                
                // Levels - extract from array columns
                nlohmann::json levels;
                nlohmann::json bids = nlohmann::json::array();
                nlohmann::json asks = nlohmann::json::array();
                
                // Get bid levels array for this row
                auto bid_array = bid_levels_col->GetAsColumn(i);
                auto bid_tuple = bid_array->As<clickhouse::ColumnTuple>();
                if (bid_tuple && bid_tuple->Size() > 0) {
                    auto bid_prices = (*bid_tuple)[0]->As<clickhouse::ColumnInt64>();
                    auto bid_sizes = (*bid_tuple)[1]->As<clickhouse::ColumnUInt32>();
                    auto bid_counts = (*bid_tuple)[2]->As<clickhouse::ColumnUInt32>();
                    
                    for (size_t j = 0; j < bid_prices->Size(); ++j) {
                        nlohmann::json level;
                        level["price"] = std::to_string(bid_prices->At(j));
                        level["size"] = bid_sizes->At(j);
                        level["count"] = bid_counts->At(j);
                        bids.push_back(level);
                    }
                }
                
                // Get ask levels array for this row
                auto ask_array = ask_levels_col->GetAsColumn(i);
                auto ask_tuple = ask_array->As<clickhouse::ColumnTuple>();
                if (ask_tuple && ask_tuple->Size() > 0) {
                    auto ask_prices = (*ask_tuple)[0]->As<clickhouse::ColumnInt64>();
                    auto ask_sizes = (*ask_tuple)[1]->As<clickhouse::ColumnUInt32>();
                    auto ask_counts = (*ask_tuple)[2]->As<clickhouse::ColumnUInt32>();
                    
                    for (size_t j = 0; j < ask_prices->Size(); ++j) {
                        nlohmann::json level;
                        level["price"] = std::to_string(ask_prices->At(j));
                        level["size"] = ask_sizes->At(j);
                        level["count"] = ask_counts->At(j);
                        asks.push_back(level);
                    }
                }
                
                levels["bids"] = bids;
                levels["asks"] = asks;
                record["levels"] = levels;
                
                // Stats
                nlohmann::json stats;
                stats["total_orders"] = total_orders_col->At(i);
                stats["bid_levels"] = bid_level_count_col->At(i);
                stats["ask_levels"] = ask_level_count_col->At(i);
                record["stats"] = stats;
                
                jsonOutput << record.dump();
            }
        });
        
        return jsonOutput.str();
        
    } catch (const std::exception& e) {
        return "{\"error\":\"Failed to build JSON: " + std::string(e.what()) + "\"}";
    }
}

} // namespace project
