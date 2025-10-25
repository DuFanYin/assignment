#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include "tcp_sender.hpp"
#include "utils.hpp"

int main() {
    std::cout << "=== TCP Market Data Sender ===" << std::endl;
    utils::logInfo("Starting TCP market data sender...");

    // Create TCP sender
    auto sender = std::make_unique<TCPSender>();
    
    // Configure sender for maximum performance
    sender->setDelayMs(0);  // No artificial delays
    sender->setZeroCopyMode(false);  // Use message-by-message mode
    sender->setPort(8080);

    // Load data file
    std::string dataFile = "/Users/hang/github_repo/assignment/src/data/CLX5_mbo.dbn";
    if (!sender->loadFromFile(dataFile)) {
        utils::logError("Failed to load data file: " + dataFile);
        return 1;
    }

    utils::logInfo("Starting TCP streaming server...");
    
    // Start streaming
    sender->startStreaming();
    
    // Wait for streaming to complete
    while (sender->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    utils::logInfo("TCP streaming completed successfully!");
    return 0;
}
