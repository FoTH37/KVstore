#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <unordered_map>

class Config{
public:
    //从文件加载配置
    bool load(const std::string& filename);

    //获取字符串配置，不存在时返回默认值
    std::string getString(const std::string& key, const std::string& defaultVal) const;

    //获取整数配置，不存在时返回默认值
    int getInt(const std::string& key, int defauleVal) const;
    
private:
    //存储所有配置项的键值对
    std::unordered_map<std::string, std::string> configs_;
};

#endif