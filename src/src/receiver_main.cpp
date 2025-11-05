#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include "../include/tcp_receiver.hpp"
#include "utils.hpp"
#include "../include/config.hpp"
#include <cstdlib>

int main() {
    Config cfg;
    const char* cfgPathEnv = std::getenv("ASSIGNMENT_CONFIG");
    std::string cfgPath = cfgPathEnv ? cfgPathEnv : std::string("/Users/hang/github_repo/assignment/src/config.ini");
    cfg.loadFromFile(cfgPath);

    // Create order book
    auto orderBook = std::make_shared<Book>();

    auto receiver = std::make_unique<TCPReceiver>();
    
    // Configure receiver from config
    receiver->setHost(cfg.getString("receiver.host", "127.0.0.1"));
    receiver->setPort(cfg.getInt("receiver.port", 8080));
    receiver->setOrderBook(orderBook);
    receiver->setSymbol(cfg.getString("receiver.symbol", "CLX5"));
    receiver->setTopLevels(cfg.getInt("receiver.top_levels", 10));
    receiver->setOutputFullBook(cfg.getBool("receiver.output_full_book", true));
    receiver->setJsonOutputFile(cfg.getString("receiver.json_output_file", "/Users/hang/github_repo/assignment/src/data/order_book_output.json"));
    
    // Configure JSON batching for optimal performance
    receiver->setJsonBatchSize(cfg.getInt("receiver.json_batch_size", 5000));
    receiver->setJsonFlushInterval(cfg.getInt("receiver.json_flush_interval", 500));

    // Connect to sender
    if (!receiver->connect()) {
        utils::logError("Failed to connect to sender");
        return 1;
    }

    // Start receiving and processing
    receiver->startReceiving();
    
    // Wait for receiving to complete
    while (receiver->isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // CRITICAL: Explicitly stop receiving to ensure final flush
    receiver->stopReceiving();
    
    // Give time for final flush to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Results (from receiver)
    std::cout << "\n=== TCP Receiver Final Statistics ===" << std::endl;
    std::cout << "Messages Received: " << receiver->getReceivedMessages() << std::endl;
    std::cout << "Orders Successfully Processed: " << receiver->getProcessedOrders() << std::endl;
    std::cout << "JSON Records Generated: " << receiver->getJsonOutputs() << std::endl;
    std::cout << "Message Throughput: " << std::fixed << std::setprecision(0) << receiver->getThroughput() << " messages/sec" << std::endl;
    double avgNs = receiver->getAverageOrderProcessNs();
    uint64_t p99Ns = receiver->getP99OrderProcessNs();
    if (avgNs > 0.0) {
        double ordersPerSec = 1e9 / avgNs;
        std::cout << "Average Order Processing Time: " << std::fixed << std::setprecision(0) << avgNs << " ns" << std::endl;
        std::cout << "P99 Order Processing Time: " << p99Ns << " ns" << std::endl;
        std::cout << "Order Processing Rate: " << std::fixed << std::setprecision(0) << ordersPerSec << " orders/sec" << std::endl;
    }
    
    // Final order book snapshot
    if (orderBook) {
        std::cout << "\nFinal Order Book Summary:" << std::endl;
        std::cout << "  Active Orders: " << orderBook->GetOrderCount() << std::endl;
        std::cout << "  Bid Price Levels: " << orderBook->GetBidLevelCount() << std::endl;
        std::cout << "  Ask Price Levels: " << orderBook->GetAskLevelCount() << std::endl;
        auto finalBbo = orderBook->Bbo();
        auto finalBid = finalBbo.second;   // first = bid
        auto finalAsk = finalBbo.first;  // second = ask
        std::cout << "  Best Bid: " << databento::pretty::Px{finalBid.price} << " @ " << finalBid.size << " (" << finalBid.count << " orders)" << std::endl;
        std::cout << "  Best Ask: " << databento::pretty::Px{finalAsk.price} << " @ " << finalAsk.size << " (" << finalAsk.count << " orders)" << std::endl;
        std::cout << "  Bid-Ask Spread: " << (finalAsk.price - finalBid.price) << std::endl;
    }
    std::cout << "=====================================" << std::endl;

    return 0;
}