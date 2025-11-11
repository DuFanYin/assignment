#pragma once

#include <string>
#include <vector>
#include <memory>
#include <clickhouse/client.h>
#include <clickhouse/columns/column.h>

/**
 * ClickHouse connection manager
 * High-performance columnar database for time-series data
 */
class ClickHouseConnection {
public:
    struct Config {
        std::string host = "localhost";
        uint16_t port = 9000;
        std::string database = "orderbook";
        std::string user = "default";
        std::string password = "";
        bool compression = true;
    };
    
    struct QueryResult {
        bool success = false;
        std::string errorMessage;
        size_t rowsAffected = 0;
    };
    
    explicit ClickHouseConnection(const Config& config);
    ~ClickHouseConnection();
    
    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const;
    bool reconnect();
    
    // Query execution
    QueryResult execute(const std::string& query);
    
    // Get underlying client for batch inserts
    clickhouse::Client* getClient() { return client_.get(); }
    
    // Utility functions
    std::string getLastError() const { return lastError_; }
    
private:
    Config config_;
    std::unique_ptr<clickhouse::Client> client_;
    bool connected_;
    std::string lastError_;
};

