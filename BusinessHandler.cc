#include "BusinessHandler.h"
#include <sstream>
#include <iostream>  // 新增
#include <vector>
#include <utility>
#include "log.h"

static void parseToken(const std::string& str, size_t& pos, std::string& token) {
    token.clear();
    // 跳过前导空格
    while (pos < str.size() && str[pos] == ' ') pos++;
    if (pos >= str.size()) return;
    if (str[pos] == '"') {
        // 引号开头：读取到下一个引号
        pos++; // 跳过开头的引号
        while (pos < str.size() && str[pos] != '"') {
            token += str[pos];
            pos++;
        }
        if (pos < str.size()) pos++; // 跳过结尾的引号
    } else {
        // 非引号开头：读取到下一个空格
        while (pos < str.size() && str[pos] != ' ') {
            token += str[pos];
            pos++;
        }
    }
}

BusinessHandler::BusinessHandler(Engine* hashtable, Engine* rbtree, Engine* skiplist,
                                 Persist* hPersist, Persist* rPersist, Persist* sPersist) {
    engines_["h"] = hashtable;
    engines_["r"] = rbtree;
    engines_["s"] = skiplist;
    persists_["h"] = hPersist;
    persists_["r"] = rPersist;
    persists_["s"] = sPersist;
}

Engine* BusinessHandler::selectEngine(const std::string& prefix){
    auto it = engines_.find(prefix);{
        if(it != engines_.end()){
            return it->second;
        }
    }
    return nullptr;
}


bool BusinessHandler::handleCommand(const std::string& data, std::string& reply) {
    LOG_DEBUG("handleCommand data size=%zu first=%.60s", data.size(), data.c_str());
    // 先尝试用 RESP 格式解析
    std::string cmd;       // 解析出的命令字符串，如 "set key value"
    std::vector<std::string> args;
    size_t consumed = 0;   // 消耗了多少字节

    if (!data.empty() && data[0] == '*') {
        // RESP 格式解析
        if (!parseRespCommand(data, cmd, args, consumed)) {
            reply = "-ERR protocol error\r\n";
            return true;
        }
    } else {
        // 文本协议解析（支持引号包裹参数）
        std::string raw = data;
        // 去掉末尾的 \r\n
        while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();

        std::vector<std::string> tokens;
        size_t pos = 0;
        while (pos < raw.size()) {
            std::string token;
            parseToken(raw, pos, token);
            if (!token.empty()) tokens.push_back(token);
        }

        if (tokens.empty()) {
            reply = "-ERR empty command\r\n";
            return true;
        }

        cmd = tokens[0];
        for (size_t i = 1; i < tokens.size(); i++) {
            args.push_back(tokens[i]);
        }
    }

    // 解析完成，统一交给已解析命令执行入口（避免调用方重复解析）
    return handleParsedCommand(cmd, args, reply);
}

// 已解析命令的执行入口：cmd 为命令名（大小写不敏感），args 为参数列表
bool BusinessHandler::handleParsedCommand(const std::string& cmd, const std::vector<std::string>& args, std::string& reply) {
    std::string op = cmd;

    // 统一转小写，简化大小写判断
    for (auto& c : op) c = tolower(c);
    // 判断引擎前缀（用完整命令名匹配）
    std::string prefix;
    std::string realCmd;
    if (op == "hset" || op == "hget" || op == "hdel") {
        prefix = "h";
        realCmd = op.substr(1);
    } else if (op == "rset" || op == "rget" || op == "rdel") {
        prefix = "r";
        realCmd = op.substr(1);
    } else if (op == "sset" || op == "sget" || op == "sdel") {
        prefix = "s";
        realCmd = op.substr(1);
    } else {
        prefix = "h";  // 默认 hashtable
        realCmd = op;
    }
    // 选择引擎
    Engine* engine = selectEngine(prefix);
    if (engine == nullptr) {
        reply = "-ERR unknown engine\r\n";
        return true;
    }
    // set
    if (realCmd == "set" || realCmd == "SET") {
        if (args.size() < 2) {
            reply = "-ERR wrong number of arguments\r\n";
            return true;
        }
        // 优化：只读引用，避免从 args 拷贝 key/value
        const std::string& key = args[0];
        const std::string& value = args[1];
        bool setResult = engine->set(key, value);
        if (setResult) {
            reply = "+OK\r\n";
            auto persistIt = persists_.find(prefix);
            if (persistIt != persists_.end() && persistIt->second) {
                // 优化：预分配 + append 链，避免多次 + 拼接的临时对象与扩容
                std::string resp;
                resp.reserve(key.size() + value.size() + 64);
                resp += "*3\r\n$3\r\nSET\r\n$";
                resp += std::to_string(key.size());
                resp += "\r\n";
                resp += key;
                resp += "\r\n$";
                resp += std::to_string(value.size());
                resp += "\r\n";
                resp += value;
                resp += "\r\n";
                persistIt->second->logCommand(resp);
                if(SyncManager_){
                    SyncManager_->broadcast(resp, key);   // key 用于发送线程哈希分片（同 key 保序）
                }
            }
        } else {
            reply = "-ERR store failed\r\n";
        }
        return true;
    }
    // get
    if (realCmd == "get" || realCmd == "GET") {
        if (args.size() < 1) {
            reply = "-ERR wrong number of arguments\r\n";
            return true;
        }
        // 优化：只读引用，避免从 args 拷贝 key
        const std::string& key = args[0];
        std::string value;
        if (engine->get(key, value)) {
            reply = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        } else {
            reply = "$-1\r\n";
        }
        return true;
    }
    // del
    if (realCmd == "del" || realCmd == "DEL") {
        if (args.size() < 1) {
            reply = "-ERR wrong number of arguments\r\n";
            return true;
        }
        // 优化：只读引用，避免从 args 拷贝 key
        const std::string& key = args[0];
        if (engine->del(key)) {
            reply = ":1\r\n";
            auto persistIt = persists_.find(prefix);
            if (persistIt != persists_.end() && persistIt->second) {
                // 优化：预分配 + append 链，避免多次 + 拼接的临时对象与扩容
                std::string resp;
                resp.reserve(key.size() + 64);
                resp += "*2\r\n$3\r\nDEL\r\n$";
                resp += std::to_string(key.size());
                resp += "\r\n";
                resp += key;
                resp += "\r\n";
                persistIt->second->logCommand(resp);
                if(SyncManager_){
                    SyncManager_->broadcast(resp, key);   // key 用于发送线程哈希分片（同 key 保序）
                }
            }
        } else {
            reply = ":0\r\n";
        }
        return true;
    }
    // save
    if (realCmd == "save" || realCmd == "SAVE") {
        for (auto& pair : persists_) {
            if (pair.second && pair.second->canSnapshot()) {
                pair.second->saveSnapshot();
            }
        }
        reply = "+OK\r\n";
        return true;
    }
    //ping
    if(realCmd == "ping" || realCmd == "PING"){
        reply = "+PONG\r\n";
        return true;
    }
    // echo：原样返回参数（纯网络回显，不碰引擎/持久化/广播——用于测网络吞吐基线）
    if (realCmd == "echo") {
        if (args.size() < 1) {
            reply = "-ERR wrong number of arguments for ECHO\r\n";
            return true;
        }
        const std::string& msg = args[0];
        reply = "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
        return true;
    }
    // mset
    if(realCmd == "mset" || realCmd == "MSET"){
        if(args.size() < 2 || args.size() % 2 != 0){
            reply = "-ERR wrong number of arguments for MSET\r\n";
            return true;
        }

        // ===== 新增：MSET 回滚逻辑 =====
        // 记录已写入的 key 和旧值，用于中途失败时回滚
        std::vector<std::pair<std::string, std::string>> rollback;  // 原本存在的 key: 旧值
        std::vector<std::string> newKeys;                            // 原本不存在的 key

        for(size_t i = 0; i < args.size(); i += 2){
            const std::string& key = args[i];
            const std::string& value = args[i + 1];

            // 备份旧值（如果 key 原本存在）
            std::string oldValue;
            bool existed = engine->get(key, oldValue);

            if(engine->set(key, value)){
                // 记录回滚信息
                if (existed) {
                    rollback.push_back({key, oldValue});
                } else {
                    newKeys.push_back(key);
                }

                auto persistIt = persists_.find(prefix);
                if(persistIt != persists_.end() && persistIt->second){
                    // 优化：预分配 + append 链，避免多次 + 拼接的临时对象与扩容
                    std::string resp;
                    resp.reserve(key.size() + value.size() + 64);
                    resp += "*3\r\n$3\r\nSET\r\n$";
                    resp += std::to_string(key.size());
                    resp += "\r\n";
                    resp += key;
                    resp += "\r\n$";
                    resp += std::to_string(value.size());
                    resp += "\r\n";
                    resp += value;
                    resp += "\r\n";
                    persistIt->second->logCommand(resp);

                    if(SyncManager_){
                        SyncManager_->broadcast(resp, key);   // key 用于发送线程哈希分片
                    }
                }
            } else {
                // ===== 新增：失败时回滚 =====
                // 1. 恢复原本存在的 key 的旧值
                for (auto& kv : rollback) {
                    engine->set(kv.first, kv.second);
                }
                // 2. 删除原本不存在的 key
                for (auto& k : newKeys) {
                    engine->del(k);
                }
                reply = "-ERR store failed for '" + key + "'\r\n";
                return true;
            }
        }
        reply = "+OK\r\n";
        return true;
    }
    // mget
    if(realCmd == "mget" || realCmd == "MGET"){
        if(args.size() < 1){
            reply = "-ERR wrong number of arguments for MGET\r\n";
            return true;
        }
        reply = "*" + std::to_string(args.size()) + "\r\n";
        for(size_t i = 0; i < args.size(); i++){
            const std::string& key = args[i];
            std::string value;
            if(engine->get(key, value)){
                reply += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
            } else {
                reply += "$-1\r\n";
            }
        }
        return true;
    }
    reply = "-ERR unknown command '" + op + "'\r\n";
    return true;
}


bool BusinessHandler::parseRespCommand(const std::string& data, std::string& cmd, std::vector<std::string>& args, size_t& consumed){
    return parseRespCommandAt(data, 0, cmd, args, consumed);
}

// 从 offset 处开始解析 RESP（性能优化：批量处理时用游标定位，
// 整批只做一次 readBuf.erase(0, pos)，避免每命令一次 O(n) 搬移）
bool BusinessHandler::parseRespCommandAt(const std::string& data, size_t offset, std::string& cmd, std::vector<std::string>& args, size_t& endPos){
    if(data.size() <= offset) return false;

    //只支持数组格式的请求(redis-cli发送的都是数组)
    if(data[offset] != '*') return false;
    size_t pos = offset + 1; //跳过'*'

    //读取数组长度(元素个数
    size_t end = data.find('\n', pos);
    if(end == std::string::npos) return false;  //数据不完整

    // 处理可能存在的 \r
    size_t len = end - pos;
    if (len > 0 && data[end - 1] == '\r') len--;
    int elemCount = std::stoi(data.substr(pos, len));
    if (elemCount <= 0 || elemCount > 1024) return false;
    //end是\r\n的位置，pos是*、$等这些字符的下一个位置，data.substr(pos, end - pos)就是截取真正有价值部分的信息
    //elemCount是数组的元素个数
    //PING → *1\r\n$4\r\nPING\r\n，     elemCount = 1
    //SET key value → *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n，     elemCount = 3

    pos = end + 1;  //跳过'\n'

    cmd = "";
    args.clear();
    for(int i = 0; i < elemCount; i++){
        //跳过可能存在的 \r 和 \n
        while (pos < data.size() && (data[pos] == '\r' || data[pos] == '\n')) pos++;

        //检查元素类型标识符
        if(pos >= data.size() || data[pos] != '$') return false;

        pos++;  //跳过'$'

        //读取字符串长度
        end = data.find('\n', pos);
        if(end == std::string::npos) return false;

        // 处理可能存在的 \r
        len = end - pos;
        if (len > 0 && data[end - 1] == '\r') len--;
        int strLen = std::stoi(data.substr(pos, len));
        if (strLen < 0 || strLen > 1024 * 1024) return false;
        pos = end + 1;  //跳过'\n'

        //检查是否有足够的数据来读取字符串内容 + "\n"
        if(pos + strLen + 1 > data.size()) return false;

        //提取字符串内容
        std::string elem = data.substr(pos, strLen);

        //第一个元素是命令名，其余是参数
        if(i == 0){
            cmd = elem;
        }else{
            args.push_back(elem);
        }

        pos += strLen + 1;  //跳过字符串内容 + "\n"
    }
    endPos = pos;  // 绝对位置（含 offset 偏移）
    return true;
}