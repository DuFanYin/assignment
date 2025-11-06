#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include "project/tcp_sender.hpp"
#include "project/utils.hpp"
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

    auto sender = std::make_unique<TCPSender>();
    std::string portStr = cfg.getString("sender.port", "");
    std::string batchSizeStr = cfg.getString("sender.batch_size", "");
    std::string dataFile = cfg.getString("sender.data_file", "");
    
    if (portStr.empty() || batchSizeStr.empty() || dataFile.empty()) {
        std::cerr << "Error: Required config keys missing: sender.port, sender.batch_size, sender.data_file" << std::endl;
        return 1;
    }
    
    sender->setPort(std::stoi(portStr));
    sender->setBatchSize(std::stoull(batchSizeStr));
    sender->setDataFile(dataFile);
    
    // Start streaming
    sender->startStreaming();
    
    // Wait for streaming to complete
    while (sender->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Results (preserved)
    std::cout << "\n=== STREAMING COMPLETED ===" << std::endl;
    long long streamingUs = sender->getStreamingUs();
    if (streamingUs < 1000) {
        std::cout << "Streaming Time:              " << std::right << std::setw(20) << streamingUs << " μs" << std::endl;
    } else {
        std::cout << "Streaming Time:              " << std::right << std::setw(20) << std::fixed << std::setprecision(3) 
                  << (streamingUs / 1000.0) << " ms" << std::endl;
    }
    std::cout << "Total Messages Sent:          " << std::right << std::setw(20) << sender->getSentMessages() << std::endl;
    std::cout << "Average Throughput:           " << std::right << std::setw(20) << std::fixed << std::setprecision(2) 
              << sender->getThroughput() << " messages/sec" << std::endl;
    std::cout << "TCP streaming completed successfully" << std::endl;
    return 0;
}
