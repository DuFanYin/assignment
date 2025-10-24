#pragma once

#include <cstdint>
#include <string>
#include <chrono>

enum class OrderSide {
    BUY,    // 'B' in data
    SELL,   // 'A' in data (Ask)
    UNKNOWN // 'N' in data
};

enum class OrderAction {
    ADD,     // 'A' - add order
    MODIFY,  // 'M' - modify order  
    CANCEL,  // 'C' - cancel order
    FILL,    // 'F' - fill order
    TRADE    // 'T' - trade summary
};

struct Order {
    uint64_t ts_event;      // Event timestamp (nanoseconds)
    uint64_t ts_recv;      // Receive timestamp (nanoseconds)
    uint8_t rtype;         // Record type
    uint16_t publisher_id; // Publisher ID
    uint32_t instrument_id;// Instrument ID
    OrderAction action;    // Action (A/M/C/F/T)
    OrderSide side;        // Side (B/A/N)
    double price;          // Order price
    uint32_t size;         // Order size
    uint16_t channel_id;   // Channel ID
    uint64_t order_id;     // Order ID
    uint8_t flags;         // Flags
    uint32_t ts_in_delta;  // Time delta
    uint32_t sequence;     // Sequence number
    std::string symbol;    // Symbol (e.g., "CLX5")

    Order() = default;
    
    Order(uint64_t ts_evt, uint64_t ts_r, uint8_t rt, uint16_t pub_id,
          uint32_t inst_id, OrderAction act, OrderSide s, double prc,
          uint32_t sz, uint16_t ch_id, uint64_t ord_id, uint8_t flgs,
          uint32_t ts_delta, uint32_t seq, const std::string& sym)
        : ts_event(ts_evt), ts_recv(ts_r), rtype(rt), publisher_id(pub_id),
          instrument_id(inst_id), action(act), side(s), price(prc),
          size(sz), channel_id(ch_id), order_id(ord_id), flags(flgs),
          ts_in_delta(ts_delta), sequence(seq), symbol(sym) {}
};
