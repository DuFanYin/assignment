#pragma once

#include <string>

namespace utils {
    // String utilities
    std::string trim(const std::string& str);

    // Logging
    void logInfo(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);
}
