#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include "core/server.hpp"
#include "util/utils.hpp"
#include <cstdlib>

int main() {
    Config cfg;
    const char* cfgPathEnv = std::getenv("ASSIGNMENT_CONFIG");
    if (!cfgPathEnv) {
        std::cerr << "Error: ASSIGNMENT_CONFIG environment variable not set" << std::endl;
        return 1;
    }
    cfg.loadFromFile(cfgPathEnv);
    
    // Get WebSocket port (default 9001)
    int wsPort = cfg.getInt("websocket.port", 9001);
    
    // Get server settings (symbol will be extracted from DBN file)
    size_t topLevels = static_cast<size_t>(cfg.getInt("server.top_levels", 10));
    
    // Get ClickHouse configuration
    ClickHouseConnection::Config dbConfig;
    dbConfig.host = cfg.getString("clickhouse.host", "localhost");
    dbConfig.port = static_cast<uint16_t>(cfg.getInt("clickhouse.port", 9000));
    dbConfig.database = cfg.getString("clickhouse.database", "orderbook");
    dbConfig.user = cfg.getString("clickhouse.user", "default");
    dbConfig.password = cfg.getString("clickhouse.password", "");
    dbConfig.compression = cfg.getBool("clickhouse.compression", true);
    
    auto server = std::make_unique<WebSocketServer>(wsPort, dbConfig, topLevels);
    
    if (!server->start()) {
        std::cerr << "Failed to start WebSocket server" << std::endl;
        return 1;
    }
    
    // Wait for interrupt
    std::this_thread::sleep_for(std::chrono::hours(24));
    
    server->stop();
    
    return 0;
}

