#pragma once
#include <cstdint>
#include <databento/record.hpp>

namespace db = databento;

// Lightweight message structure for ring buffer
struct MboMessage {
    uint64_t ts_event;
    uint64_t ts_recv;
    uint8_t rtype;
    uint16_t publisher_id;
    uint32_t instrument_id;
    uint8_t action;
    uint8_t side;
    int64_t price;
    uint32_t size;
    uint8_t channel_id;
    uint64_t order_id;
    uint8_t flags;
    int32_t ts_in_delta;
    uint32_t sequence;
} __attribute__((packed));

// Convert databento MboMsg to our lightweight structure
inline MboMessage convertToMboMessage(const db::MboMsg& mbo) {
    MboMessage msg;
    msg.ts_event = mbo.hd.ts_event.time_since_epoch().count();
    msg.ts_recv = mbo.ts_recv.time_since_epoch().count(); // ts_recv is not in header
    msg.rtype = static_cast<uint8_t>(mbo.hd.rtype);
    msg.publisher_id = mbo.hd.publisher_id;
    msg.instrument_id = mbo.hd.instrument_id;
    msg.action = static_cast<uint8_t>(mbo.action);
    msg.side = static_cast<uint8_t>(mbo.side);
    msg.price = mbo.price;
    msg.size = mbo.size;
    msg.channel_id = mbo.channel_id;
    msg.order_id = mbo.order_id;
    msg.flags = static_cast<uint8_t>(mbo.flags); // Try implicit conversion
    msg.ts_in_delta = mbo.ts_in_delta.count();
    msg.sequence = mbo.sequence;
    return msg;
}

// Convert our lightweight structure back to databento MboMsg
inline db::MboMsg convertToDatabentoMbo(const MboMessage& msg) {
    db::MboMsg mbo;
    
    // Convert timestamps - use the same approach as tcp_receiver.cpp
    auto epoch_time = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(msg.ts_event)));
    mbo.hd.ts_event = epoch_time;
    
    auto recv_time = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(msg.ts_recv)));
    mbo.ts_recv = recv_time;
    
    // Convert header fields
    mbo.hd.rtype = static_cast<db::RType>(msg.rtype);
    mbo.hd.publisher_id = msg.publisher_id;
    mbo.hd.instrument_id = msg.instrument_id;
    
    // Convert order fields
    mbo.action = static_cast<db::Action>(msg.action);
    mbo.side = static_cast<db::Side>(msg.side);
    mbo.price = msg.price;
    mbo.size = msg.size;
    mbo.channel_id = msg.channel_id;
    mbo.order_id = msg.order_id;
    mbo.flags = db::FlagSet(msg.flags); // Use FlagSet constructor
    mbo.ts_in_delta = std::chrono::nanoseconds(msg.ts_in_delta);
    mbo.sequence = msg.sequence;
    
    return mbo;
}
