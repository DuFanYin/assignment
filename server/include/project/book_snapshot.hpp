#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <databento/constants.hpp>
#include "project/order_book.hpp"

namespace db = databento;

// Snapshot types for database/JSON generation
struct LevelEntry {
    int64_t price{db::kUndefPrice};
    uint32_t size{0};
    uint32_t count{0};
};

struct BookSnapshot {
    std::string symbol;
    int64_t ts_ns{0};
    PriceLevel bid{};
    PriceLevel ask{};
    std::vector<LevelEntry> bids; // top N bid levels, highest first
    std::vector<LevelEntry> asks; // top N ask levels, lowest first
    size_t total_orders{0};
    size_t bid_levels{0};
    size_t ask_levels{0};
};

