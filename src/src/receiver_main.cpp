#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include "tcp_receiver.hpp"
#include "utils.hpp"

int main() {
    std::cout << "=== TCP Market Data Receiver with Order Book ===" << std::endl;
    utils::logInfo("Starting TCP market data receiver...");

    // Create order book
    auto orderBook = std::make_shared<Book>();

    // Create TCP receiver
    auto receiver = std::make_unique<TCPReceiver>();
    
    // Configure receiver
    receiver->setHost("127.0.0.1");
    receiver->setPort(8080);
    receiver->setOrderBook(orderBook);

    utils::logInfo("Connecting to TCP sender...");
    
    // Connect to sender
    if (!receiver->connect()) {
        utils::logError("Failed to connect to sender");
        return 1;
    }

    utils::logInfo("Starting message reception and order book processing...");
    
    // Start receiving and processing
    receiver->startReceiving();
    
    // Wait for receiving to complete
    while (receiver->isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    utils::logInfo("TCP reception and order book processing completed successfully!");
    return 0;
}
