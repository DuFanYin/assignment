#include "database/postgres_connection.hpp"
#include "project/utils.hpp"
#include <sstream>
#include <cstring>

PostgresConnection::PostgresConnection(const Config& config)
    : config_(config)
    , connection_(nullptr)
    , inTransaction_(false) {
}

PostgresConnection::~PostgresConnection() {
    disconnect();
}

bool PostgresConnection::connect() {
    if (connection_ && isConnected()) {
        return true;
    }
    
    // Build connection string
    std::stringstream connInfo;
    connInfo << "host=" << config_.host
             << " port=" << config_.port
             << " dbname=" << config_.dbname
             << " user=" << config_.user
             << " password=" << config_.password
             << " connect_timeout=" << config_.connectionTimeout;
    
    connection_ = PQconnectdb(connInfo.str().c_str());
    
    if (PQstatus(connection_) != CONNECTION_OK) {
        lastError_ = PQerrorMessage(connection_);
        utils::logError("PostgreSQL connection failed: " + lastError_);
        PQfinish(connection_);
        connection_ = nullptr;
        return false;
    }
    
    utils::logInfo("Connected to PostgreSQL database: " + config_.dbname);
    return true;
}

void PostgresConnection::disconnect() {
    if (connection_) {
        if (inTransaction_) {
            rollbackTransaction();
        }
        PQfinish(connection_);
        connection_ = nullptr;
    }
}

bool PostgresConnection::isConnected() const {
    return connection_ && PQstatus(connection_) == CONNECTION_OK;
}

bool PostgresConnection::reconnect() {
    disconnect();
    return connect();
}

bool PostgresConnection::checkConnection() {
    if (!isConnected()) {
        return reconnect();
    }
    return true;
}

PostgresConnection::QueryResult PostgresConnection::execute(const std::string& query) {
    if (!checkConnection()) {
        QueryResult result;
        result.success = false;
        result.errorMessage = "Not connected to database";
        return result;
    }
    
    PGresult* pgResult = PQexec(connection_, query.c_str());
    QueryResult result = processResult(pgResult);
    clearResult(pgResult);
    
    return result;
}

PostgresConnection::QueryResult PostgresConnection::executeParams(const std::string& query, 
                                                                  const std::vector<std::string>& params) {
    if (!checkConnection()) {
        QueryResult result;
        result.success = false;
        result.errorMessage = "Not connected to database";
        return result;
    }
    
    // Convert parameters to C strings
    std::vector<const char*> paramValues;
    for (const auto& param : params) {
        paramValues.push_back(param.c_str());
    }
    
    PGresult* pgResult = PQexecParams(
        connection_,
        query.c_str(),
        static_cast<int>(params.size()),
        nullptr,  // paramTypes
        paramValues.data(),
        nullptr,  // paramLengths
        nullptr,  // paramFormats
        0         // resultFormat (text)
    );
    
    QueryResult result = processResult(pgResult);
    clearResult(pgResult);
    
    return result;
}

bool PostgresConnection::beginTransaction() {
    if (!checkConnection()) {
        return false;
    }
    
    if (inTransaction_) {
        lastError_ = "Already in a transaction";
        return false;
    }
    
    PGresult* result = PQexec(connection_, "BEGIN");
    bool success = PQresultStatus(result) == PGRES_COMMAND_OK;
    
    if (success) {
        inTransaction_ = true;
    } else {
        lastError_ = PQerrorMessage(connection_);
    }
    
    clearResult(result);
    return success;
}

bool PostgresConnection::commitTransaction() {
    if (!checkConnection()) {
        return false;
    }
    
    if (!inTransaction_) {
        lastError_ = "Not in a transaction";
        return false;
    }
    
    PGresult* result = PQexec(connection_, "COMMIT");
    bool success = PQresultStatus(result) == PGRES_COMMAND_OK;
    
    if (success) {
        inTransaction_ = false;
    } else {
        lastError_ = PQerrorMessage(connection_);
    }
    
    clearResult(result);
    return success;
}

bool PostgresConnection::rollbackTransaction() {
    if (!checkConnection()) {
        return false;
    }
    
    if (!inTransaction_) {
        return true;  // Nothing to rollback
    }
    
    PGresult* result = PQexec(connection_, "ROLLBACK");
    bool success = PQresultStatus(result) == PGRES_COMMAND_OK;
    
    inTransaction_ = false;  // Reset transaction state even if rollback fails
    
    if (!success) {
        lastError_ = PQerrorMessage(connection_);
    }
    
    clearResult(result);
    return success;
}

bool PostgresConnection::prepareStatement(const std::string& stmtName, const std::string& query) {
    if (!checkConnection()) {
        return false;
    }
    
    PGresult* result = PQprepare(connection_, stmtName.c_str(), query.c_str(), 0, nullptr);
    bool success = PQresultStatus(result) == PGRES_COMMAND_OK;
    
    if (!success) {
        lastError_ = PQerrorMessage(connection_);
    }
    
    clearResult(result);
    return success;
}

PostgresConnection::QueryResult PostgresConnection::executePrepared(const std::string& stmtName,
                                                                    const std::vector<std::string>& params) {
    if (!checkConnection()) {
        QueryResult result;
        result.success = false;
        result.errorMessage = "Not connected to database";
        return result;
    }
    
    // Convert parameters to C strings
    std::vector<const char*> paramValues;
    for (const auto& param : params) {
        paramValues.push_back(param.c_str());
    }
    
    PGresult* pgResult = PQexecPrepared(
        connection_,
        stmtName.c_str(),
        static_cast<int>(params.size()),
        paramValues.data(),
        nullptr,  // paramLengths
        nullptr,  // paramFormats
        0         // resultFormat (text)
    );
    
    QueryResult result = processResult(pgResult);
    clearResult(pgResult);
    
    return result;
}

std::string PostgresConnection::escapeString(const std::string& input) const {
    if (!connection_) {
        return input;
    }
    
    std::vector<char> escaped(input.size() * 2 + 1);
    PQescapeStringConn(connection_, escaped.data(), input.c_str(), input.size(), nullptr);
    
    return std::string(escaped.data());
}

std::string PostgresConnection::getLastError() const {
    return lastError_;
}

void PostgresConnection::clearResult(PGresult* result) {
    if (result) {
        PQclear(result);
    }
}

PostgresConnection::QueryResult PostgresConnection::processResult(PGresult* result) {
    QueryResult queryResult;
    
    if (!result) {
        queryResult.success = false;
        queryResult.errorMessage = "Null result";
        return queryResult;
    }
    
    ExecStatusType status = PQresultStatus(result);
    
    switch (status) {
        case PGRES_COMMAND_OK:
            queryResult.success = true;
            queryResult.rowsAffected = std::atoi(PQcmdTuples(result));
            break;
            
        case PGRES_TUPLES_OK: {
            queryResult.success = true;
            int nRows = PQntuples(result);
            int nCols = PQnfields(result);
            
            // Get column names
            for (int col = 0; col < nCols; ++col) {
                queryResult.columnNames.push_back(PQfname(result, col));
            }
            
            // Get row data
            for (int row = 0; row < nRows; ++row) {
                std::vector<std::string> rowData;
                for (int col = 0; col < nCols; ++col) {
                    if (PQgetisnull(result, row, col)) {
                        rowData.push_back("");
                    } else {
                        rowData.push_back(PQgetvalue(result, row, col));
                    }
                }
                queryResult.rows.push_back(rowData);
            }
            queryResult.rowsAffected = nRows;
            break;
        }
        
        default:
            queryResult.success = false;
            queryResult.errorMessage = PQerrorMessage(connection_);
            lastError_ = queryResult.errorMessage;
            break;
    }
    
    return queryResult;
}

// Transaction RAII wrapper implementation
Transaction::Transaction(PostgresConnection& conn)
    : conn_(conn)
    , committed_(false)
    , active_(false) {
    active_ = conn_.beginTransaction();
}

Transaction::~Transaction() {
    if (active_ && !committed_) {
        conn_.rollbackTransaction();
    }
}

bool Transaction::commit() {
    if (!active_) {
        return false;
    }
    
    bool success = conn_.commitTransaction();
    if (success) {
        committed_ = true;
        active_ = false;
    }
    return success;
}

void Transaction::rollback() {
    if (active_) {
        conn_.rollbackTransaction();
        active_ = false;
    }
}

// Connection pooling stub (can be implemented later)
std::shared_ptr<PostgresConnection> PostgresConnection::getPooledConnection(const Config& config) {
    auto conn = std::make_shared<PostgresConnection>(config);
    if (conn->connect()) {
        return conn;
    }
    return nullptr;
}

void PostgresConnection::releaseToPool() {
    // Stub for connection pooling
}
