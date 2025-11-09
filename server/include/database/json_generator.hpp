#pragma once

#include "database/postgres_connection.hpp"
#include <string>
#include <memory>

namespace project {

class JSONGenerator {
public:
    explicit JSONGenerator(const PostgresConnection::Config& config);
    ~JSONGenerator();
    
    // Generate JSON from database for a specific session
    std::string generateJSON(const std::string& sessionId);
    
    // Generate JSON from database for a specific symbol (latest session)
    std::string generateJSONForSymbol(const std::string& symbol);
    
    // Get database connection (for status checks)
    PostgresConnection& getConnection() { return conn_; }
    
private:
    PostgresConnection conn_;
    
    // Helper to build JSON from query results
    std::string buildJSON(const PostgresConnection::QueryResult& snapshotsRes);
    std::string fetchLevels(const std::string& snapshotId, const std::string& tableName);
};

} // namespace project
