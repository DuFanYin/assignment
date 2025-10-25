#include "utils.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace utils {

std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()) % 1000000000;
    
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(9) << ns.count() << "+00:00";
    return ss.str();
}

uint64_t timestampToNanoseconds(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point nanosecondsToTimestamp(uint64_t ns) {
    // Convert nanoseconds to system_clock's duration type
    auto duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(ns));
    return std::chrono::system_clock::time_point(duration);
}

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    
    return tokens;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "";
    }
    
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
}

bool startsWith(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && 
           str.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() && 
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

double roundToDecimalPlaces(double value, int decimalPlaces) {
    double multiplier = pow(10.0, decimalPlaces);
    return round(value * multiplier) / multiplier;
}

bool isValidPrice(double price) {
    return price > 0.0 && std::isfinite(price);
}

bool isValidQuantity(uint32_t quantity) {
    return quantity > 0;
}

bool fileExists(const std::string& filePath) {
    std::ifstream file(filePath);
    return file.good();
}

size_t getFileSize(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return 0;
    }
    return file.tellg();
}

std::string readFileToString(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void logInfo(const std::string& message) {
    std::cout << "[INFO] " << message << std::endl;
}

void logWarning(const std::string& message) {
    std::cout << "[WARNING] " << message << std::endl;
}

void logError(const std::string& message) {
    std::cerr << "[ERROR] " << message << std::endl;
}

void logDebug(const std::string& message) {
    std::cout << "[DEBUG] " << message << std::endl;
}

} // namespace utils
