#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <thread>
#include "tcp_receiver.hpp"
#include "utils.hpp"

int main() {
    std::cout << "=== TCP Market Data Receiver with Order Book ===" << std::endl;
    std::cout << "📊 High-Performance Order Book Reconstruction & JSON Output" << std::endl;
    std::cout << "=========================================================" << std::endl;
    
    utils::logInfo("Starting TCP market data receiver...");

    // Create order book
    auto orderBook = std::make_shared<Book>();

    // Create TCP receiver
    auto receiver = std::make_unique<TCPReceiver>();
    
    // Configure receiver
    receiver->setHost("127.0.0.1");
    receiver->setPort(8080);
    receiver->setOrderBook(orderBook);
    
    // Enable JSON output for order book state
    receiver->setSymbol("CLX5");
    receiver->setTopLevels(10);
    receiver->setOutputFullBook(true);
    receiver->enableJsonOutput(true);
    receiver->setJsonOutputFile("../data/order_book_output.json");

    std::cout << "🌐 Server Host: 127.0.0.1" << std::endl;
    std::cout << "🔌 Server Port: 8080" << std::endl;
    std::cout << "📈 Symbol: CLX5" << std::endl;
    std::cout << "📊 Top Levels: 10" << std::endl;
    std::cout << "📋 Output Mode: Complete Order Book" << std::endl;
    std::cout << "📁 JSON Output File: ../data/order_book_output.json" << std::endl;
    std::cout << std::endl;

    utils::logInfo("Connecting to TCP sender...");
    
    // Connect to sender
    if (!receiver->connect()) {
        utils::logError("Failed to connect to sender");
        return 1;
    }

    utils::logInfo("Starting message reception and order book processing with JSON output...");
    
    // Start receiving and processing
    receiver->startReceiving();
    
    // Wait for receiving to complete
    while (receiver->isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Print comprehensive final summary
    std::cout << "\n=== PROCESSING COMPLETED ===" << std::endl;
    std::cout << "📊 Total Messages Received: " << receiver->getReceivedMessages() << std::endl;
    std::cout << "📊 Total Orders Processed: " << receiver->getProcessedOrders() << std::endl;
    std::cout << "📊 JSON Outputs Generated: " << receiver->getJsonOutputs() << std::endl;
    std::cout << "📈 Average Throughput: " << std::fixed << std::setprecision(2) 
              << receiver->getThroughput() << " messages/sec" << std::endl;
    std::cout << "📁 JSON Output File: ../data/order_book_output.json" << std::endl;
    std::cout << "✅ TCP reception and order book processing completed successfully!" << std::endl;
    return 0;
}
