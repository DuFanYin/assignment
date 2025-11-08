#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include "tcp_sender.hpp"
#include "utils.hpp"

int main() {
    std::cout << "=== TCP Market Data Sender ===" << std::endl;
    std::cout << "🚀 High-Performance Market Data Streaming Server" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    utils::logInfo("Starting TCP market data sender...");

    // Create TCP sender
    auto sender = std::make_unique<TCPSender>();
    
    // Configure sender for maximum performance
    sender->setDelayMs(0);  // No artificial delays
    sender->setZeroCopyMode(false);  // Use message-by-message mode
    sender->setPort(8080);

    // Load data file
    std::string dataFile = "data/CLX5_mbo.dbn";
    if (!sender->loadFromFile(dataFile)) {
        utils::logError("Failed to load data file: " + dataFile);
        return 1;
    }

    std::cout << "📁 Data File: " << dataFile << std::endl;
    std::cout << "🌐 Server Port: 8080" << std::endl;
    std::cout << std::endl;

    utils::logInfo("Starting TCP streaming server...");
    
    // Start streaming
    sender->startStreaming();
    
    // Wait for streaming to complete
    while (sender->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Print comprehensive final summary
    std::cout << "\n=== STREAMING COMPLETED ===" << std::endl;
    std::cout << "📊 Total Messages Sent: " << sender->getSentMessages() << std::endl;
    std::cout << "📈 Average Throughput: " << std::fixed << std::setprecision(2) 
              << sender->getThroughput() << " messages/sec" << std::endl;
    std::cout << "✅ TCP streaming completed successfully!" << std::endl;
    return 0;
}
