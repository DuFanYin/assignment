#include "project/utils.hpp"
#include <iostream>
#include <string>

namespace utils {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "";
    }
    
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
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

} // namespace utils
