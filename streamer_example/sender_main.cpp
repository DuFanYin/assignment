#include "tcp_sender.hpp"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>

// Global sender for signal handling
TCPSender* g_sender = nullptr;

void signalHandler(int signal) {
    if (g_sender) {
        std::cout << "\n🛑 Received signal " << signal << ", stopping..." << std::endl;
        g_sender->stopStreaming();
        // Give some time for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    exit(0);
}

int main() {
    std::cout << "=== TCP Market Data Sender ===" << std::endl;
    
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create sender
    auto sender = std::make_unique<TCPSender>();
    g_sender = sender.get();
    
    // Configure sender for batch streaming
    sender->setHost("127.0.0.1");
    sender->setPort(8080);
    sender->setDelayMs(0); // NO DELAY - Maximum speed
    sender->setBatchMode(true); // Enable batch streaming
    sender->setBatchSize(1000); // Send 1000 messages per batch
    
    // Load data file (same as our main processor)
    std::string dataFile = "/Users/hang/github_repo/assignment/src/data/CLX5_mbo.dbn";
    if (!sender->loadFromFile(dataFile)) {
        std::cerr << "❌ Failed to load data file: " << dataFile << std::endl;
        std::cerr << "💡 Make sure the file exists and is readable" << std::endl;
        return 1;
    }
    
    // Start streaming with retry mechanism
    int retryCount = 0;
    const int maxRetries = 3;
    
    while (!sender->startStreaming() && retryCount < maxRetries) {
        retryCount++;
        std::cerr << "❌ Failed to start streaming (attempt " << retryCount << "/" << maxRetries << ")" << std::endl;
        
        if (retryCount < maxRetries) {
            std::cout << "⏳ Waiting 2 seconds before retry..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    
    if (retryCount >= maxRetries) {
        std::cerr << "❌ Failed to start streaming after " << maxRetries << " attempts" << std::endl;
        std::cerr << "💡 Try running: lsof -i :8080" << std::endl;
        std::cerr << "💡 Or kill any processes using port 8080" << std::endl;
        return 1;
    }
    
    // Keep running until stopped
    std::cout << "🚀 TCP sender running... Press Ctrl+C to stop" << std::endl;
    while (sender->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Print final statistics
    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Total Orders Sent: " << sender->getSentOrders() << std::endl;
    std::cout << "Connected Clients: " << sender->getConnectedClients() << std::endl;
    std::cout << "=====================" << std::endl;
    
    // Explicitly stop streaming before destruction
    sender->stopStreaming();
    
    std::cout << "✅ TCP sender stopped" << std::endl;
    return 0;
}
