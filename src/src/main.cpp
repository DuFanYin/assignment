#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
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
    
    // Mutex to ensure sequential processing and prevent race conditions
    std::mutex orderMutex;

    // Create streamer
    auto streamer = std::make_unique<Streamer>();

    // Set up order callback with proper synchronization
    streamer->setOrderCallback([&orderBook, &orderMutex](const Order& order) {
        // Lock to ensure sequential processing
        std::lock_guard<std::mutex> lock(orderMutex);
        
        try {
            // Convert Order back to MboMsg for the new order book
            databento::MboMsg mbo;
            
            // Convert nanoseconds to system_clock time_point properly
            auto epoch = std::chrono::system_clock::time_point{};
            mbo.hd.ts_event = epoch + std::chrono::nanoseconds(order.ts_event);
            mbo.hd.publisher_id = order.publisher_id;
            mbo.hd.instrument_id = order.instrument_id;
            mbo.hd.rtype = static_cast<databento::RType>(order.rtype);
            mbo.order_id = order.order_id;
            mbo.price = static_cast<int64_t>(order.price * 100000000); // Convert to price ticks
            mbo.size = order.size;
            mbo.flags = databento::FlagSet(order.flags);
            mbo.channel_id = order.channel_id;
            
            // Convert action and side properly
            char actionChar;
            switch (order.action) {
                case OrderAction::ADD: actionChar = 'A'; break;
                case OrderAction::MODIFY: actionChar = 'M'; break;
                case OrderAction::CANCEL: actionChar = 'C'; break;
                case OrderAction::FILL: actionChar = 'F'; break;
                case OrderAction::TRADE: actionChar = 'T'; break;
                default: actionChar = 'A'; break;
            }
            
            char sideChar;
            switch (order.side) {
                case OrderSide::BUY: sideChar = 'B'; break;
                case OrderSide::SELL: sideChar = 'A'; break;
                case OrderSide::UNKNOWN: sideChar = 'N'; break;
                default: sideChar = 'N'; break;
            }
            
            mbo.action = static_cast<databento::Action>(actionChar);
            mbo.side = static_cast<databento::Side>(sideChar);
            mbo.ts_recv = epoch + std::chrono::nanoseconds(order.ts_recv);
            mbo.ts_in_delta = std::chrono::nanoseconds(order.ts_in_delta);
            mbo.sequence = order.sequence;
            
            // Process order through order book with graceful error handling
            try {
                orderBook->Apply(mbo);
            } catch (const std::invalid_argument& e) {
                // Handle missing orders/levels gracefully
                std::string error_msg = e.what();
                if (error_msg.find("No order with ID") != std::string::npos ||
                    error_msg.find("Received event for unknown level") != std::string::npos) {
                    // Skip orders that reference non-existent orders/levels
                    // This is common in real market data due to data gaps
                    utils::logWarning("Skipping order due to missing reference: " + error_msg);
                    return;
                }
                // Re-throw other invalid_argument exceptions
                throw;
            }
            
            // Print order book stats every 100 orders
            static int orderCount = 0;
            if (++orderCount % 100 == 0) {
                std::cout << "\n--- Order Book Status ---" << std::endl;
                std::cout << "Total Orders: " << orderBook->GetOrderCount() << std::endl;
                std::cout << "Bid Levels: " << orderBook->GetBidLevelCount() << std::endl;
                std::cout << "Ask Levels: " << orderBook->GetAskLevelCount() << std::endl;
                
                auto bbo = orderBook->Bbo();
                std::cout << "Best Bid: " << db::pretty::Px{bbo.first.price} << " @ " << bbo.first.size << " (" << bbo.first.count << " orders)" << std::endl;
                std::cout << "Best Ask: " << db::pretty::Px{bbo.second.price} << " @ " << bbo.second.size << " (" << bbo.second.count << " orders)" << std::endl;
                std::cout << "Spread: " << (bbo.second.price - bbo.first.price) << std::endl;
                std::cout << "------------------------" << std::endl;
            }
        } catch (const std::exception& e) {
            utils::logError("Error processing order: " + std::string(e.what()));
        }
    });

    // Load data file
    std::string dataFile = "../data/CLX5_mbo.dbn";
    if (!streamer->loadFromFile(dataFile)) {
        utils::logError("Failed to load data file: " + dataFile);
        return 1;
    }

    // Start streaming
    utils::logInfo("Starting data stream...");
    streamer->startStreaming();

    // Keep running until streaming stops
    while (streamer->isStreaming()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final statistics
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Processed Orders: " << streamer->getProcessedOrders() << std::endl;
    std::cout << "Final Order Book:" << std::endl;
    std::cout << "  Total Orders: " << orderBook->GetOrderCount() << std::endl;
    std::cout << "  Bid Levels: " << orderBook->GetBidLevelCount() << std::endl;
    std::cout << "  Ask Levels: " << orderBook->GetAskLevelCount() << std::endl;
    
    auto finalBbo = orderBook->Bbo();
    std::cout << "  Best Bid: " << db::pretty::Px{finalBbo.first.price} << " @ " << finalBbo.first.size << " (" << finalBbo.first.count << " orders)" << std::endl;
    std::cout << "  Best Ask: " << db::pretty::Px{finalBbo.second.price} << " @ " << finalBbo.second.size << " (" << finalBbo.second.count << " orders)" << std::endl;
    std::cout << "  Spread: " << (finalBbo.second.price - finalBbo.first.price) << std::endl;
    std::cout << "========================" << std::endl;

    utils::logInfo("Market data processing completed successfully!");
    return 0;
}