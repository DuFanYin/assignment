#include "streamer.hpp"
#include "utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>

Streamer::Streamer() 
    : streaming_(false), processedOrders_(0) {
}

void Streamer::setOrderCallback(OrderCallback callback) {
    orderCallback_ = callback;
}

bool Streamer::loadFromFile(const std::string& filePath) {
    filePath_ = filePath;
    
    try {
        store_ = std::make_unique<databento::DbnFileStore>(filePath);
        auto meta = store_->GetMetadata();
        
        utils::logInfo("Loaded DBN file: " + filePath);
        utils::logInfo("Schema: " + std::string(databento::ToString(meta.schema.value())));
        utils::logInfo("Dataset: " + meta.dataset);
        
        return true;
    } catch (const std::exception& e) {
        utils::logError("Failed to open DBN file: " + filePath + " - " + e.what());
        return false;
    }
}

void Streamer::startStreaming() {
    if (streaming_) {
        utils::logWarning("Streaming already in progress");
        return;
    }
    
    if (!store_) {
        utils::logError("No file loaded");
        return;
    }
    
    streaming_ = true;
    utils::logInfo("Starting data stream...");
    
    try {
        while (streaming_) {
            const auto* record = store_->NextRecord();
            if (!record) break; // End of file
            
            if (record->RType() == databento::RType::Mbo) {
                const auto& mbo = record->Get<databento::MboMsg>();
                
                if (orderCallback_) {
                    // Convert MboMsg to Order for backward compatibility
                    Order order = convertMboToOrder(mbo);
                    orderCallback_(order);
                    processedOrders_++;
                }
            }
            
            // Increased delay to prevent race conditions and ensure sequential processing
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) {
        utils::logError("Error during streaming: " + std::string(e.what()));
    }
    
    streaming_ = false;
    utils::logInfo("Streaming completed. Processed " + std::to_string(processedOrders_) + " orders.");
}

void Streamer::stopStreaming() {
    streaming_ = false;
    utils::logInfo("Stopping data stream...");
}

const databento::Record* Streamer::getNextRecord() {
    if (!store_) {
        return nullptr;
    }
    return store_->NextRecord();
}

Order Streamer::convertMboToOrder(const databento::MboMsg& mbo) {
    Order order;
    
    order.ts_event = mbo.hd.ts_event.time_since_epoch().count();
    order.ts_recv = mbo.ts_recv.time_since_epoch().count();
    order.rtype = static_cast<uint8_t>(mbo.hd.rtype);
    order.publisher_id = mbo.hd.publisher_id;
    order.instrument_id = mbo.hd.instrument_id;
    order.action = parseAction(static_cast<char>(mbo.action));
    order.side = parseSide(static_cast<char>(mbo.side));
    order.price = mbo.price;
    order.size = mbo.size;
    order.channel_id = mbo.channel_id;
    order.order_id = mbo.order_id;
    order.flags = static_cast<uint8_t>(mbo.flags);
    order.ts_in_delta = mbo.ts_in_delta.count();
    order.sequence = mbo.sequence;
    order.symbol = "CLX5"; // We know this from the metadata
    
    return order;
}

OrderSide Streamer::parseSide(char side) {
    switch (side) {
        case 'B': return OrderSide::BUY;
        case 'A': return OrderSide::SELL;
        case 'N': return OrderSide::UNKNOWN;
        default: return OrderSide::UNKNOWN;
    }
}

OrderAction Streamer::parseAction(char action) {
    switch (action) {
        case 'A': return OrderAction::ADD;
        case 'M': return OrderAction::MODIFY;
        case 'C': return OrderAction::CANCEL;
        case 'F': return OrderAction::FILL;
        case 'T': return OrderAction::TRADE;
        default: return OrderAction::ADD;
    }
}
