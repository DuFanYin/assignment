#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include "project/server.hpp"
#include "project/config.hpp"
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
    size_t ringBufferSize = static_cast<size_t>(cfg.getInt("server.ring_buffer_size", 65536));
    
    // Get PostgreSQL configuration
    PostgresConnection::Config dbConfig;
    dbConfig.host = cfg.getString("postgres.host", "localhost");
    dbConfig.port = cfg.getInt("postgres.port", 5432);
    dbConfig.dbname = cfg.getString("postgres.dbname", "orderbook");
    dbConfig.user = cfg.getString("postgres.user", "postgres");
    dbConfig.password = cfg.getString("postgres.password", "postgres");
    dbConfig.maxConnections = cfg.getInt("postgres.max_connections", 10);
    dbConfig.connectionTimeout = cfg.getInt("postgres.connection_timeout", 30);
    
    auto server = std::make_unique<WebSocketServer>(wsPort, dbConfig, topLevels, ringBufferSize);
    
    if (!server->start()) {
        std::cerr << "Failed to start WebSocket server" << std::endl;
        return 1;
    }
    
    // Wait for interrupt
    std::this_thread::sleep_for(std::chrono::hours(24));
    
    server->stop();
    
    return 0;
}

