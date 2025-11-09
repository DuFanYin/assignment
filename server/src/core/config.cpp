#include "project/config.hpp"
#include "project/utils.hpp"
#include <fstream>
#include <algorithm>

bool Config::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = utils::trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = utils::trim(line.substr(0, pos));
        std::string value = utils::trim(line.substr(pos + 1));
        kv_[key] = value;
    }
    return true;
}

std::string Config::getString(const std::string& key, const std::string& def) const {
    auto it = kv_.find(key);
    return it == kv_.end() ? def : it->second;
}

int Config::getInt(const std::string& key, int def) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

bool Config::getBool(const std::string& key, bool def) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return def;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}
