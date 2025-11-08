#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace utils {
    // Time utilities
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp);
    uint64_t timestampToNanoseconds(const std::chrono::system_clock::time_point& tp);
    std::chrono::system_clock::time_point nanosecondsToTimestamp(uint64_t ns);

    // String utilities
    std::vector<std::string> split(const std::string& str, char delimiter);
    std::string trim(const std::string& str);
    bool startsWith(const std::string& str, const std::string& prefix);
    bool endsWith(const std::string& str, const std::string& suffix);

    // Numeric utilities
    double roundToDecimalPlaces(double value, int decimalPlaces);
    bool isValidPrice(double price);
    bool isValidQuantity(uint32_t quantity);

    // File utilities
    bool fileExists(const std::string& filePath);
    size_t getFileSize(const std::string& filePath);
    std::string readFileToString(const std::string& filePath);

    // Logging
    void logInfo(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);
    void logDebug(const std::string& message);
}
