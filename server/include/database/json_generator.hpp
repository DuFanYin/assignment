#pragma once

#include "database/clickhouse_connection.hpp"
#include <string>
#include <memory>

namespace project {

class JSONGenerator {
public:
    explicit JSONGenerator(const ClickHouseConnection::Config& config);
    ~JSONGenerator();
    
    // Generate JSON from database for a specific session
    std::string generateJSON(const std::string& sessionId);
    
    // Generate JSON from database for a specific symbol (latest session)
    std::string generateJSONForSymbol(const std::string& symbol);
    
    // Get database connection (for status checks)
    ClickHouseConnection& getConnection() { return clickhouseConnection_; }
    
private:
    ClickHouseConnection clickhouseConnection_;
    
    // Helper to build JSON from ClickHouse results
    std::string buildJSON(const std::string& sessionId);
};

} // namespace project
