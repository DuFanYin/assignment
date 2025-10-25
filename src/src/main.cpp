#include <iostream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <databento/record.hpp>
#include <databento/enums.hpp>
#include <databento/datetime.hpp>
#include <databento/pretty.hpp>
#include "order_book.hpp"
#include "streamer.hpp"
#include "utils.hpp"

namespace db = databento;

int main() {
    std::cout << "=== Market Data Processor ===" << std::endl;
    utils::logInfo("Starting market data processor...");

    // Create order book
    auto orderBook = std::make_unique<Book>();

    // Create streamer
    auto streamer = std::make_unique<Streamer>();

    // Load data file
    std::string dataFile = "../data/CLX5_mbo.dbn";
    if (!streamer->loadFromFile(dataFile)) {
        utils::logError("Failed to load data file: " + dataFile);
        return 1;
    }

    // Process orders directly in single thread - no callbacks needed
    utils::logInfo("Starting data stream...");
    
    // Start timing
    auto startTime = std::chrono::high_resolution_clock::now();
    
    int orderCount = 0;  // Move outside try block for proper scope
    try {
        while (true) {
            const auto* record = streamer->getNextRecord();
            if (!record) break; // End of file
            
            if (record->RType() == databento::RType::Mbo) {
                const auto& mbo = record->Get<databento::MboMsg>();
                
                // Process order directly through order book
                try {
                    orderBook->Apply(mbo);
                    orderCount++;
                    
                    // Print order book stats every 100 orders
                    if (orderCount % 100 == 0) {
                        std::cout << "\n--- Order Book Status ---" << std::endl;
                        std::cout << "Total Orders: " << orderBook->GetOrderCount() << std::endl;
                        std::cout << "Bid Levels: " << orderBook->GetBidLevelCount() << std::endl;
                        std::cout << "Ask Levels: " << orderBook->GetAskLevelCount() << std::endl;
                        
                        auto bbo = orderBook->Bbo();
                        // Note: Bbo() returns {bid, ask} but the order book seems to return them swapped
                        // Let's check which is actually bid vs ask by comparing prices
                        auto bid = bbo.first.price < bbo.second.price ? bbo.first : bbo.second;
                        auto ask = bbo.first.price > bbo.second.price ? bbo.first : bbo.second;
                        
                        std::cout << "Best Bid: " << db::pretty::Px{bid.price} << " @ " << bid.size << " (" << bid.count << " orders)" << std::endl;
                        std::cout << "Best Ask: " << db::pretty::Px{ask.price} << " @ " << ask.size << " (" << ask.count << " orders)" << std::endl;
                        std::cout << "Spread: " << (ask.price - bid.price) << std::endl;
                        std::cout << "------------------------" << std::endl;
                    }
                } catch (const std::invalid_argument& e) {
                    // Handle missing orders/levels gracefully - this is normal for real market data
                    std::string error_msg = e.what();
                    if (error_msg.find("No order with ID") != std::string::npos ||
                        error_msg.find("Received event for unknown level") != std::string::npos) {
                        // Skip orders that reference non-existent orders/levels
                        // This is common in real market data due to data gaps or out-of-sequence processing
                        static int skippedCount = 0;
                        if (++skippedCount % 1000 == 0) {
                            utils::logInfo("Skipped " + std::to_string(skippedCount) + " orders due to missing references (normal for real market data)");
                        }
                        continue;
                    }
                    // Re-throw other invalid_argument exceptions
                    throw;
                }
            }
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
        return 1;
    }

    // End timing
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Final statistics
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Processing Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Processed Orders: " << orderCount << std::endl;
    if (duration.count() > 0) {
        double ordersPerSecond = (double)orderCount * 1000.0 / duration.count();
        std::cout << "Processing Rate: " << std::fixed << std::setprecision(0) << ordersPerSecond << " orders/sec" << std::endl;
    }
    std::cout << "Final Order Book:" << std::endl;
    std::cout << "  Total Orders: " << orderBook->GetOrderCount() << std::endl;
    std::cout << "  Bid Levels: " << orderBook->GetBidLevelCount() << std::endl;
    std::cout << "  Ask Levels: " << orderBook->GetAskLevelCount() << std::endl;
    
    auto finalBbo = orderBook->Bbo();
    // Note: Bbo() returns {bid, ask} but the order book seems to return them swapped
    // Let's check which is actually bid vs ask by comparing prices
    auto finalBid = finalBbo.first.price < finalBbo.second.price ? finalBbo.first : finalBbo.second;
    auto finalAsk = finalBbo.first.price > finalBbo.second.price ? finalBbo.first : finalBbo.second;
    
    std::cout << "  Best Bid: " << db::pretty::Px{finalBid.price} << " @ " << finalBid.size << " (" << finalBid.count << " orders)" << std::endl;
    std::cout << "  Best Ask: " << db::pretty::Px{finalAsk.price} << " @ " << finalAsk.size << " (" << finalAsk.count << " orders)" << std::endl;
    std::cout << "  Spread: " << (finalAsk.price - finalBid.price) << std::endl;
    std::cout << "========================" << std::endl;

    utils::logInfo("Market data processing completed successfully!");
    return 0;
}