#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <cstddef>

#include "util/order_book.hpp"

namespace utils {
    // String utilities
    std::string trim(const std::string& str);

    // Logging
    void logInfo(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);

    struct MboMessageWrapper {
        BookSnapshot snapshot;
        std::chrono::steady_clock::time_point timestamp;

        MboMessageWrapper();
        explicit MboMessageWrapper(const BookSnapshot& snap);
    };
}

using MboMessageWrapper = utils::MboMessageWrapper;

struct SessionStats {
    size_t messagesReceived = 0;
    size_t ordersProcessed = 0;
    double throughput = 0.0;
    int64_t avgProcessNs = 0;
    uint64_t p99ProcessNs = 0;
    size_t totalOrders = 0;
    size_t bidLevels = 0;
    size_t askLevels = 0;
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double spread = 0.0;
    bool hasBookState = false;
};

class Config {
public:
    bool loadFromFile(const std::string& path);

    std::string getString(const std::string& key, const std::string& def) const;
    int getInt(const std::string& key, int def) const;
    bool getBool(const std::string& key, bool def) const;

private:
    std::unordered_map<std::string, std::string> kv_;
};
