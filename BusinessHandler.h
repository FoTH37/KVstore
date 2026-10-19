#ifndef BUSINESS_HANDLER_H
#define BUSINESS_HANDLER_H

#include <string>
#include <vector>
#include <unordered_map>
#include "engine.h"
#include "persist.h"
#include "sync_manager.h"


class BusinessHandler{
public:
    BusinessHandler(Engine* hashtable, Engine* rbtree, Engine* skiplist,
                    Persist* hPersist, Persist* rPersist, Persist* sPersist);
    ~BusinessHandler() = default;

    bool handleCommand(const std::string& cmd, std::string& reply);

    // 已解析命令的执行入口（解析一次、执行一次，避免重复解析）
    bool handleParsedCommand(const std::string& cmd, const std::vector<std::string>& args, std::string& reply);

    static bool parseRespCommand(const std::string& data, std::string& cmd, std::vector<std::string>& args, size_t& consumed);

    // 从 offset 处开始解析 RESP（批量处理时用游标，避免每命令一次 erase 搬移）
    static bool parseRespCommandAt(const std::string& data, size_t offset, std::string& cmd, std::vector<std::string>& args, size_t& endPos);

    SyncManager* getSyncManager() {return SyncManager_;}

    void setSyncManager(SyncManager* sm) {SyncManager_ = sm;}

private:
    Engine* selectEngine(const std::string& prefix);
    std::unordered_map<std::string, Engine*> engines_;
    std::unordered_map<std::string, Persist*> persists_;

    SyncManager* SyncManager_ = nullptr;
};

#endif