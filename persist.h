#ifndef PERSIST_H
#define PERSIST_H

#include <string>
#include <new>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include "engine.h"

// 持久化模式枚举
enum PersistMode {
    PERSIST_MODE_RDB = 0,   // 仅 RDB 快照，关闭 AOF
    PERSIST_MODE_AOF = 1,   // 仅 AOF 增量，关闭 RDB
    PERSIST_MODE_BOTH = 2   // 两者都开（默认）
};

class Persist {
public:
    Persist(const std::string& filename, Engine* engine, PersistMode mode = PERSIST_MODE_BOTH);
    ~Persist();

    void logCommand(const std::string& cmd);
    void recover();

    // RDB 相关
    void saveSnapshot();
    void loadSnapshot();
    void setRdbInterval(int seconds);  // 设置自动快照间隔（秒），0 表示不自动
    // 判断当前模式是否支持 RDB 快照（rdb 或 both）
    // aof 模式下返回 false，SAVE 命令不会生成 rdb 文件
    bool canSnapshot() const {
        return mode_ == PERSIST_MODE_RDB || mode_ == PERSIST_MODE_BOTH;
    }

private:
    void flushThread();
    void rotate(bool is_aof);

    std::string filename_;
    std::string rdb_filename_;
    Engine* engine_;
    PersistMode mode_;

    // AOF 队列
    std::deque<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;

    std::thread flush_thread_;
    int fd_ = -1;

    // RDB 相关
    bool rdb_dirty_ = false;
    int rdb_interval_ = 30;
    time_t last_rdb_time_ = 0;

    static const size_t MAX_FILE_SIZE = 512 * 1024 * 1024;
    static const int MAX_BACKUP = 2;
};

#endif