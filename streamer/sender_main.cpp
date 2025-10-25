#include "tcp_sender.hpp"
#include <iostream>
#include <signal.h>

// Global sender for signal handling
TCPSender* g_sender = nullptr;

void signalHandler(int signal) {
    if (g_sender) {
        std::cout << "\n🛑 Received signal " << signal << ", stopping..." << std::endl;
        g_sender->stopStreaming();
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
    
    // Configure sender
    sender->setHost("127.0.0.1");
    sender->setPort(8080);
    sender->setDelayMs(0); // NO DELAY - Maximum speed
    sender->setZeroCopyMode(false); // Use message-by-message mode for proper parsing
    
    // Load data file (same as our main processor)
    std::string dataFile = "/Users/hang/github_repo/assignment/src/data/CLX5_mbo.dbn";
    if (!sender->loadFromFile(dataFile)) {
        std::cerr << "❌ Failed to load data file: " << dataFile << std::endl;
        return 1;
    }
    
    // Start streaming
    if (!sender->startStreaming()) {
        std::cerr << "❌ Failed to start streaming" << std::endl;
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
    
    std::cout << "✅ TCP sender stopped" << std::endl;
    return 0;
}
