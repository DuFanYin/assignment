#include "util/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <ostream>
#include <ranges>
#include <stdexcept>

namespace {
constexpr const char* kUnknownLevelError = "Received event for unknown level ";
}

bool PriceLevel::IsEmpty() const {
    return price == db::kUndefPrice;
}

PriceLevel::operator bool() const {
    return !IsEmpty();
}

std::ostream& operator<<(std::ostream& stream, const PriceLevel& level) {
    stream << level.size << " @ " << db::pretty::Px{level.price} << " | "
           << level.count << " order(s)";
    return stream;
}

void Book::setSymbol(const std::string& symbol) {
    symbol_ = symbol;
}

void Book::setTopLevels(size_t levels) {
    topLevels_ = levels;
}

std::pair<PriceLevel, PriceLevel> Book::Bbo() const {
    return {GetBidLevel(), GetAskLevel()};
}

PriceLevel Book::GetBidLevel(std::size_t idx) const {
    if (bids_.size() > idx) {
        auto level_it = bids_.rbegin();
        std::advance(level_it, idx);
        return GetPriceLevel(level_it->first, level_it->second);
    }
    return {};
}

PriceLevel Book::GetAskLevel(std::size_t idx) const {
    if (offers_.size() > idx) {
        auto level_it = offers_.begin();
        std::advance(level_it, idx);
        return GetPriceLevel(level_it->first, level_it->second);
    }
    return {};
}

PriceLevel Book::GetBidLevelByPx(int64_t px) const {
    auto level_it = bids_.find(px);
    if (level_it == bids_.end()) {
        throw std::invalid_argument{"No bid level at " + db::pretty::PxToString(px)};
    }
    return GetPriceLevel(px, level_it->second);
}

PriceLevel Book::GetAskLevelByPx(int64_t px) const {
    auto level_it = offers_.find(px);
    if (level_it == offers_.end()) {
        throw std::invalid_argument{"No ask level at " + db::pretty::PxToString(px)};
    }
    return GetPriceLevel(px, level_it->second);
}

const db::MboMsg& Book::GetOrder(uint64_t order_id) {
    auto order_it = orders_by_id_.find(order_id);
    if (order_it == orders_by_id_.end()) {
        throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
    }
    auto& level = GetLevel(order_it->second.side, order_it->second.price);
    return *GetLevelOrder(level, order_id);
}

uint32_t Book::GetQueuePos(uint64_t order_id) {
    auto order_it = orders_by_id_.find(order_id);
    if (order_it == orders_by_id_.end()) {
        throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
    }
    const auto& level_it = GetLevel(order_it->second.side, order_it->second.price);
    uint32_t prior_size = 0;
    for (const auto& order : level_it) {
        if (order.order_id == order_id) {
            break;
        }
        prior_size += order.size;
    }
    return prior_size;
}

std::vector<db::BidAskPair> Book::GetSnapshot(std::size_t level_count) const {
    std::vector<db::BidAskPair> res;
    for (size_t i = 0; i < level_count; ++i) {
        db::BidAskPair ba_pair{db::kUndefPrice, db::kUndefPrice, 0, 0, 0, 0};
        auto bid = GetBidLevel(i);
        if (bid) {
            ba_pair.bid_px = bid.price;
            ba_pair.bid_sz = bid.size;
            ba_pair.bid_ct = bid.count;
        }
        auto ask = GetAskLevel(i);
        if (ask) {
            ba_pair.ask_px = ask.price;
            ba_pair.ask_sz = ask.size;
            ba_pair.ask_ct = ask.count;
        }
        res.emplace_back(ba_pair);
    }
    return res;
}

size_t Book::GetOrderCount() const {
    return orders_by_id_.size();
}

size_t Book::GetBidLevelCount() const {
    return bids_.size();
}

size_t Book::GetAskLevelCount() const {
    return offers_.size();
}

void Book::Apply(const db::MboMsg& mbo) {
    switch (mbo.action) {
        case db::Action::Clear:
            Clear();
            break;
        case db::Action::Add:
            Add(mbo);
            break;
        case db::Action::Cancel:
            Cancel(mbo);
            break;
        case db::Action::Modify:
            Modify(mbo);
            break;
        case db::Action::Trade:
        case db::Action::Fill:
        case db::Action::None:
            break;
        default:
            throw std::invalid_argument{std::string{"Unknown action: "} + db::ToString(mbo.action)};
    }
}

void Book::Clear() {
    orders_by_id_.clear();
    offers_.clear();
    bids_.clear();
}

PriceLevel Book::GetPriceLevel(int64_t price, const LevelOrders level) {
    PriceLevel res{price};
    for (const auto& order : level) {
        if (!order.flags.IsTob()) {
            ++res.count;
        }
        res.size += order.size;
    }
    return res;
}

Book::LevelOrders::iterator Book::GetLevelOrder(LevelOrders& level, uint64_t order_id) {
    auto order_it = std::ranges::find_if(level, [order_id](const db::MboMsg& order) {
        return order.order_id == order_id;
    });
    if (order_it == level.end()) {
        throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
    }
    return order_it;
}

void Book::Add(db::MboMsg mbo) {
    if (mbo.flags.IsTob()) {
        SideLevels& levels = GetSideLevels(mbo.side);
        levels.clear();
        if (mbo.price != db::kUndefPrice) {
            LevelOrders level = {mbo};
            levels.emplace(mbo.price, level);
        }
    } else {
        LevelOrders& level = GetOrInsertLevel(mbo.side, mbo.price);
        level.emplace_back(mbo);
        auto res = orders_by_id_.emplace(mbo.order_id, PriceAndSide{mbo.price, mbo.side});
        if (!res.second) {
            throw std::invalid_argument{"Received duplicated order ID " + std::to_string(mbo.order_id)};
        }
    }
}

void Book::Cancel(db::MboMsg mbo) {
    LevelOrders& level = GetLevel(mbo.side, mbo.price);
    auto order_it = GetLevelOrder(level, mbo.order_id);
    if (order_it->size < mbo.size) {
        throw std::logic_error{"Tried to cancel more size than existed for order ID " +
                               std::to_string(mbo.order_id)};
    }
    order_it->size -= mbo.size;
    if (order_it->size == 0) {
        orders_by_id_.erase(mbo.order_id);
        level.erase(order_it);
        if (level.empty()) {
            RemoveLevel(mbo.side, mbo.price);
        }
    }
}

void Book::Modify(db::MboMsg mbo) {
    auto price_side_it = orders_by_id_.find(mbo.order_id);
    if (price_side_it == orders_by_id_.end()) {
        Add(mbo);
        return;
    }
    if (price_side_it->second.side != mbo.side) {
        throw std::logic_error{"Order " + std::to_string(mbo.order_id) + " changed side"};
    }
    auto prev_price = price_side_it->second.price;
    LevelOrders& prev_level = GetLevel(mbo.side, prev_price);
    auto level_order_it = GetLevelOrder(prev_level, mbo.order_id);
    if (prev_price != mbo.price) {
        price_side_it->second.price = mbo.price;
        prev_level.erase(level_order_it);
        if (prev_level.empty()) {
            RemoveLevel(mbo.side, prev_price);
        }
        LevelOrders& level = GetOrInsertLevel(mbo.side, mbo.price);
        level.emplace_back(mbo);
    } else if (level_order_it->size < mbo.size) {
        LevelOrders& level = prev_level;
        level.erase(level_order_it);
        level.emplace_back(mbo);
    } else {
        level_order_it->size = mbo.size;
    }
}

Book::SideLevels& Book::GetSideLevels(db::Side side) {
    switch (side) {
        case db::Side::Ask:
            return offers_;
        case db::Side::Bid:
            return bids_;
        case db::Side::None:
        default:
            throw std::invalid_argument{"Invalid side"};
    }
}

Book::LevelOrders& Book::GetLevel(db::Side side, int64_t price) {
    SideLevels& levels = GetSideLevels(side);
    auto level_it = levels.find(price);
    if (level_it == levels.end()) {
        throw std::invalid_argument{std::string{kUnknownLevelError} + db::ToString(side) +
                                    " " + db::pretty::PxToString(price)};
    }
    return level_it->second;
}

Book::LevelOrders& Book::GetOrInsertLevel(db::Side side, int64_t price) {
    SideLevels& levels = GetSideLevels(side);
    return levels[price];
}

void Book::RemoveLevel(db::Side side, int64_t price) {
    SideLevels& levels = GetSideLevels(side);
    levels.erase(price);
}

const std::vector<Market::PublisherBook>& Market::GetBooksByPub(uint32_t instrument_id) {
    return books_[instrument_id];
}

const Book& Market::GetBook(uint32_t instrument_id, uint16_t publisher_id) {
    const std::vector<PublisherBook>& books = GetBooksByPub(instrument_id);
    auto book_it = std::ranges::find_if(books, [publisher_id](const PublisherBook& pub_book) {
        return pub_book.publisher_id == publisher_id;
    });
    if (book_it == books.end()) {
        throw std::invalid_argument{"No book for publisher ID " + std::to_string(publisher_id)};
    }
    return book_it->book;
}

std::pair<PriceLevel, PriceLevel> Market::Bbo(uint32_t instrument_id, uint16_t publisher_id) {
    const auto& book = GetBook(instrument_id, publisher_id);
    return book.Bbo();
}

std::pair<PriceLevel, PriceLevel> Market::AggregatedBbo(uint32_t instrument_id) {
    PriceLevel agg_bid;
    PriceLevel agg_ask;
    for (const auto& pub_book : GetBooksByPub(instrument_id)) {
        const auto bbo = pub_book.book.Bbo();
        const auto& bid = bbo.first;
        const auto& ask = bbo.second;
        if (bid) {
            if (agg_bid.IsEmpty() || bid.price > agg_bid.price) {
                agg_bid = bid;
            } else if (bid.price == agg_bid.price) {
                agg_bid.count += bid.count;
                agg_bid.size += bid.size;
            }
        }
        if (ask) {
            if (agg_ask.IsEmpty() || ask.price < agg_ask.price) {
                agg_ask = ask;
            } else if (ask.price == agg_ask.price) {
                agg_ask.count += ask.count;
                agg_ask.size += ask.size;
            }
        }
    }
    return {agg_bid, agg_ask};
}

void Market::Apply(const db::MboMsg& mbo_msg) {
    auto& instrument_books = books_[mbo_msg.hd.instrument_id];
    auto book_it = std::ranges::find_if(instrument_books, [&mbo_msg](const PublisherBook& pub_book) {
        return pub_book.publisher_id == mbo_msg.hd.publisher_id;
    });
    if (book_it == instrument_books.end()) {
        instrument_books.emplace_back(PublisherBook{mbo_msg.hd.publisher_id, {}});
        book_it = std::prev(instrument_books.end());
    }
    book_it->book.Apply(mbo_msg);
}


