#pragma once

#include <cstdint>

#include <databento/constants.hpp>
#include <databento/enums.hpp>
#include <databento/pretty.hpp>
#include <databento/record.hpp>

#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace db = databento;

struct LevelEntry {
    int64_t price{db::kUndefPrice};
    uint32_t size{0};
    uint32_t count{0};
};

struct PriceLevel {
    int64_t price{db::kUndefPrice};
    uint32_t size{0};
    uint32_t count{0};

    bool IsEmpty() const;
    explicit operator bool() const;
};

std::ostream& operator<<(std::ostream& stream, const PriceLevel& level);

struct BookSnapshot {
    std::string symbol;
    int64_t ts_ns{0};
    PriceLevel bid{};
    PriceLevel ask{};
    std::vector<LevelEntry> bids;
    std::vector<LevelEntry> asks;
    size_t total_orders{0};
    size_t bid_levels{0};
    size_t ask_levels{0};
};

class Book {
public:
    void setSymbol(const std::string& symbol);
    void setTopLevels(size_t levels);

    std::pair<PriceLevel, PriceLevel> Bbo() const;

    PriceLevel GetBidLevel(std::size_t idx = 0) const;
    PriceLevel GetAskLevel(std::size_t idx = 0) const;
    PriceLevel GetBidLevelByPx(int64_t px) const;
    PriceLevel GetAskLevelByPx(int64_t px) const;
    const db::MboMsg& GetOrder(uint64_t order_id);
    uint32_t GetQueuePos(uint64_t order_id);
    std::vector<db::BidAskPair> GetSnapshot(std::size_t level_count = 1) const;

    size_t GetOrderCount() const;
    size_t GetBidLevelCount() const;
    size_t GetAskLevelCount() const;

    void Apply(const db::MboMsg& mbo);
    void Clear();

private:
    using LevelOrders = std::vector<db::MboMsg>;
    struct PriceAndSide {
        int64_t price;
        db::Side side;
    };
    using Orders = std::unordered_map<uint64_t, PriceAndSide>;
    using SideLevels = std::map<int64_t, LevelOrders>;

    static PriceLevel GetPriceLevel(int64_t price, const LevelOrders level);
    static LevelOrders::iterator GetLevelOrder(LevelOrders& level, uint64_t order_id);

    void Add(db::MboMsg mbo);
    void Cancel(db::MboMsg mbo);
    void Modify(db::MboMsg mbo);

    SideLevels& GetSideLevels(db::Side side);
    LevelOrders& GetLevel(db::Side side, int64_t price);
    LevelOrders& GetOrInsertLevel(db::Side side, int64_t price);
    void RemoveLevel(db::Side side, int64_t price);

    std::string symbol_;
    size_t topLevels_{0};
    Orders orders_by_id_;
    SideLevels offers_;
    SideLevels bids_;
};

class Market {
public:
    struct PublisherBook {
        uint16_t publisher_id;
        Book book;
    };

    const std::vector<PublisherBook>& GetBooksByPub(uint32_t instrument_id);
    const Book& GetBook(uint32_t instrument_id, uint16_t publisher_id);
    std::pair<PriceLevel, PriceLevel> Bbo(uint32_t instrument_id, uint16_t publisher_id);
    std::pair<PriceLevel, PriceLevel> AggregatedBbo(uint32_t instrument_id);
    void Apply(const db::MboMsg& mbo_msg);

private:
    std::unordered_map<uint32_t, std::vector<PublisherBook>> books_;
};

