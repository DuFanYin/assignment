#pragma once

#include <string>
#include <vector>
#include <libpq-fe.h>

/**
 * PostgreSQL connection manager
 * Thread-safe connection pooling for high-performance database access
 */
class PostgresConnection {
public:
    struct Config {
        std::string host = "localhost";
        int port = 5432;
        std::string dbname;
        std::string user;
        std::string password;
        int maxConnections = 10;
        int connectionTimeout = 30;
    };
    
    struct QueryResult {
        bool success = false;
        std::string errorMessage;
        int rowsAffected = 0;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> columnNames;
    };
    
    explicit PostgresConnection(const Config& config);
    ~PostgresConnection();
    
    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const;
    bool reconnect();
    bool checkConnection();  // Check and auto-reconnect if needed
    
    // Query execution
    QueryResult execute(const std::string& query);
    QueryResult executeParams(const std::string& query, const std::vector<std::string>& params);
    
    // Transaction support
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    
    // Prepared statements
    bool prepareStatement(const std::string& stmtName, const std::string& query);
    QueryResult executePrepared(const std::string& stmtName, const std::vector<std::string>& params);
    
    // COPY command support for bulk loading
    bool beginCopy(const std::string& tableName, const std::vector<std::string>& columns);
    bool putCopyData(const std::string& data);
    bool endCopy();
    
    // Utility functions
    std::string escapeString(const std::string& input) const;
    std::string getLastError() const;
    
private:
    Config config_;
    PGconn* connection_;
    bool inTransaction_;
    std::string lastError_;
    
    void clearResult(PGresult* result);
    QueryResult processResult(PGresult* result);
};

