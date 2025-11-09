#pragma once

#include <string>
#include <unordered_map>

class Config {
public:
    bool loadFromFile(const std::string& path);

    std::string getString(const std::string& key, const std::string& def) const;
    int getInt(const std::string& key, int def) const;
    bool getBool(const std::string& key, bool def) const;

private:
    std::unordered_map<std::string, std::string> kv_;
};

