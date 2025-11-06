#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <thread>
#include "project/tcp_receiver.hpp"
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

    // Create order book
    auto orderBook = std::make_shared<Book>();

    auto receiver = std::make_unique<TCPReceiver>();
    
    // Configure receiver from config
    std::string host = cfg.getString("receiver.host", "");
    std::string portStr = cfg.getString("receiver.port", "");
    std::string symbol = cfg.getString("receiver.symbol", "");
    std::string topLevelsStr = cfg.getString("receiver.top_levels", "");
    std::string outputFullBookStr = cfg.getString("receiver.output_full_book", "");
    std::string jsonOutputFile = cfg.getString("receiver.json_output_file", "");
    std::string jsonBatchSizeStr = cfg.getString("receiver.json_batch_size", "");
    std::string jsonFlushIntervalStr = cfg.getString("receiver.json_flush_interval", "");
    
    if (host.empty() || portStr.empty() || symbol.empty() || topLevelsStr.empty() ||
        jsonOutputFile.empty() || jsonBatchSizeStr.empty() || jsonFlushIntervalStr.empty()) {
        std::cerr << "Error: Required config keys missing in receiver section" << std::endl;
        return 1;
    }
    
    receiver->setHost(host);
    receiver->setPort(std::stoi(portStr));
    receiver->setOrderBook(orderBook);
    receiver->setSymbol(symbol);
    receiver->setTopLevels(std::stoull(topLevelsStr));
    receiver->setOutputFullBook(outputFullBookStr == "true" || outputFullBookStr == "1");
    receiver->setJsonOutputFile(jsonOutputFile);
    receiver->setJsonBatchSize(std::stoull(jsonBatchSizeStr));
    receiver->setJsonFlushInterval(std::stoull(jsonFlushIntervalStr));

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
    std::cout << "Messages Received:           " << std::right << std::setw(20) << receiver->getReceivedMessages() << std::endl;
    std::cout << "Orders Successfully Processed:" << std::right << std::setw(20) << receiver->getProcessedOrders() << std::endl;
    std::cout << "JSON Records Generated:       " << std::right << std::setw(20) << receiver->getJsonOutputs() << std::endl;
    std::cout << "Message Throughput:           " << std::right << std::setw(20) << std::fixed << std::setprecision(0) << receiver->getThroughput() << " messages/sec" << std::endl;
    double avgNs = receiver->getAverageOrderProcessNs();
    uint64_t p99Ns = receiver->getP99OrderProcessNs();
    if (avgNs > 0.0) {
        double ordersPerSec = 1e9 / avgNs;
        std::cout << "Average Order Processing Time:" << std::right << std::setw(20) << std::fixed << std::setprecision(0) << avgNs << " ns" << std::endl;
        std::cout << "P99 Order Processing Time:   " << std::right << std::setw(20) << p99Ns << " ns" << std::endl;
        std::cout << "Order Processing Rate:       " << std::right << std::setw(20) << std::fixed << std::setprecision(0) << ordersPerSec << " orders/sec" << std::endl;
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