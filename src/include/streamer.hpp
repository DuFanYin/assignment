#pragma once

#include <string>
#include <functional>
#include <memory>
#include <databento/dbn_file_store.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>
#include "order.hpp"

class Streamer {
public:
    using OrderCallback = std::function<void(const Order&)>;

    Streamer();
    ~Streamer() = default;

    // Configuration
    void setOrderCallback(OrderCallback callback);

    // Data streaming
    bool loadFromFile(const std::string& filePath);
    void startStreaming();
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // Statistics
    size_t getProcessedOrders() const { return processedOrders_; }
    
    // Single-threaded access method
    const databento::Record* getNextRecord();

private:
    OrderCallback orderCallback_;
    bool streaming_;
    size_t processedOrders_;
    std::unique_ptr<databento::DbnFileStore> store_;
    std::string filePath_;

    // Data processing
    Order convertMboToOrder(const databento::MboMsg& mbo);
    OrderSide parseSide(char side);
    OrderAction parseAction(char action);
};
