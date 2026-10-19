#include "config.h"
#include <fstream>
#include <iostream>
#include <sstream>

bool Config::load(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;  // 文件不存在，返回 false，调用者使用默认配置
    }

    std::string line;
    while (std::getline(file, line)) {
        // 跳过空行
        if (line.empty()) continue;

        // 跳过注释行（# 开头）
        if (line[0] == '#') continue;

        // 找 = 的位置
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;  // 没有 =，跳过

        // 分割 key 和 value
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // 去除 key 两端的空格
        size_t start = key.find_first_not_of(" \t");
        size_t end = key.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            key = key.substr(start, end - start + 1);
        }

        // 去除 value 两端的空格
        start = value.find_first_not_of(" \t");
        end = value.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            value = value.substr(start, end - start + 1);
        }

        // 存入配置表
        configs_[key] = value;
    }

    file.close();
    return true;
}

std::string Config::getString(const std::string& key, const std::string& defaultVal) const {
    auto it = configs_.find(key);
    if (it != configs_.end()) {
        return it->second;
    }
    return defaultVal;
}

int Config::getInt(const std::string& key, int defaultVal) const {
    auto it = configs_.find(key);
    if (it != configs_.end()) {
        // ===== 修改：捕获异常，非法数字返回默认值 =====
        // 配置文件里如果写成了非数字（如 port=abc），std::stoi 会抛异常导致崩溃
        try {
            return std::stoi(it->second);
        } catch (...) {
            return defaultVal;  // 非法数字，返回默认值，不崩溃
        }
    }
    return defaultVal;
}