#pragma once

#include <cstdint>
#include <string>
#include <chrono>

struct Trade {
    uint64_t tradeId;
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    std::string symbol;
    uint32_t quantity;
    double price;
    std::chrono::system_clock::time_point timestamp;
    std::string buyClientId;
    std::string sellClientId;

    Trade(uint64_t id, uint64_t buyId, uint64_t sellId, const std::string& sym,
          uint32_t qty, double prc, const std::string& buyClient, const std::string& sellClient)
        : tradeId(id), buyOrderId(buyId), sellOrderId(sellId), symbol(sym),
          quantity(qty), price(prc), timestamp(std::chrono::system_clock::now()),
          buyClientId(buyClient), sellClientId(sellClient) {}
};
