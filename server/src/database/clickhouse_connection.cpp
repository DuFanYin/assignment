#include "database/clickhouse_connection.hpp"
#include <iostream>

ClickHouseConnection::ClickHouseConnection(const Config& config)
    : config_(config)
    , client_(nullptr)
    , connected_(false)
    , lastError_("") {
}

ClickHouseConnection::~ClickHouseConnection() {
    disconnect();
}

bool ClickHouseConnection::connect() {
    try {
        // Create client options
        clickhouse::ClientOptions options;
        options.SetHost(config_.host);
        options.SetPort(config_.port);
        options.SetUser(config_.user);
        options.SetPassword(config_.password);
        options.SetDefaultDatabase(config_.database);
        if (config_.compression) {
            options.SetCompressionMethod(clickhouse::CompressionMethod::LZ4);
        }
        // Attempt with current options
        try {
            client_ = std::make_unique<clickhouse::Client>(options);
            client_->Execute("SELECT 1");
            connected_ = true;
            lastError_.clear();
            return true;
        } catch (const std::exception& e1) {
            // Fallback: retry without compression (some builds/envs lack LZ4 linkage)
            try {
                clickhouse::ClientOptions noComp = options;
                noComp.SetCompressionMethod(clickhouse::CompressionMethod::None);
                client_ = std::make_unique<clickhouse::Client>(noComp);
                client_->Execute("SELECT 1");
                connected_ = true;
                lastError_.clear();
                return true;
            } catch (const std::exception& e2) {
                lastError_ = std::string("Primary: ") + e1.what() + " | Fallback: " + e2.what();
                connected_ = false;
                return false;
            }
        }
    } catch (const std::exception& e) {
        lastError_ = std::string("Connection init failed: ") + e.what();
        connected_ = false;
        return false;
    }
}

void ClickHouseConnection::disconnect() {
    client_.reset();
    connected_ = false;
}

bool ClickHouseConnection::isConnected() const {
    return connected_ && client_ != nullptr;
}

bool ClickHouseConnection::reconnect() {
    disconnect();
    return connect();
}

ClickHouseConnection::QueryResult ClickHouseConnection::execute(const std::string& query) {
    QueryResult result;
    
    if (!isConnected()) {
        result.success = false;
        result.errorMessage = "Not connected to ClickHouse";
        lastError_ = result.errorMessage;
        return result;
    }
    
    try {
        client_->Execute(query);
        result.success = true;
        result.rowsAffected = 0; // ClickHouse doesn't return affected rows for DDL
        lastError_.clear();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Query failed: ") + e.what();
        lastError_ = result.errorMessage;
    }
    
    return result;
}

