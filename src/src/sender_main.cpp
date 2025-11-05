#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include "tcp_sender.hpp"
#include "utils.hpp"
#include "../include/config.hpp"
#include <cstdlib>

int main() {
    Config cfg;
    const char* cfgPathEnv = std::getenv("ASSIGNMENT_CONFIG");
    std::string cfgPath = cfgPathEnv ? cfgPathEnv : std::string("/Users/hang/github_repo/assignment/src/config.ini");
    cfg.loadFromFile(cfgPath);

    auto sender = std::make_unique<TCPSender>();
    sender->setPort(cfg.getInt("sender.port", 8080));
    sender->setBatchSize(cfg.getInt("sender.batch_size", 100));
    
    // Start streaming
    sender->startStreaming();
    
    // Wait for streaming to complete
    while (sender->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Results (preserved)
    std::cout << "\n=== STREAMING COMPLETED ===" << std::endl;
    std::cout << "Streaming Time: " << sender->getStreamingMs() << " ms" << std::endl;
    std::cout << "Total Messages Sent: " << sender->getSentMessages() << std::endl;
    std::cout << "Average Throughput: " << std::fixed << std::setprecision(2) 
              << sender->getThroughput() << " messages/sec" << std::endl;
    std::cout << "TCP streaming completed successfully" << std::endl;
    return 0;
}
