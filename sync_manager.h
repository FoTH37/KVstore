#ifndef SYNC_MANAGER_H
#define SYNC_MANAGER_H

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unistd.h>

struct SlaveInfo {
    int fd;
    bool full_sync_done = false;
};

// 异步广播管理器（方案A：广播异步化 + key 分片多发送线程）
//   worker 侧 broadcast(cmd, key) 只做"持锁入队"（临界区 = 判空 + push，微秒级），
//   按 key 哈希分片到 sync_senders 个队列，每个队列一个独立发送线程。
//   发送线程在【锁外】逐条 write 到从库连接。
//
//   设计要点：
//   1. 单发送线程时同步吞吐受单核 write+eBPF 开销限制（~1 万条/秒），
//      key 分片多线程可突破（不同 key 互不影响，可乱序；同 key 同队列保序）
//   2. eBPF 捕获要求一次 write = 一条命令（BPF 只取第一条），必须逐条 write
//   3. 发送线程数由 kvstore.conf 的 sync_senders 配置，main() 启动时 startSenders(n)
class SyncManager {
public:
    SyncManager() = default;   // 不启动线程；main() 读配置后调用 startSenders(n)
    ~SyncManager();
    void startSenders(int n);              // 启动 n 个发送线程（n<=0 视为 1）
    void addSlave(int fd);
    void removeSlave(int fd);
    void markFullSyncDone(int fd);
    void broadcast(const std::string& cmd, const std::string& key);  // worker 热路径：按 key 入队
private:
    void sendLoop(int idx);                // 第 idx 个发送线程入口
    std::vector<SlaveInfo> slaves_;        // 从库列表（mutex_ 保护）
    std::vector<std::deque<std::string>> pending_;   // 每发送线程一个队列（mutex_ 保护）
    // condition_variable 不可拷贝/移动，须用 unique_ptr 包装后入 vector
    std::vector<std::unique_ptr<std::condition_variable>> cvs_;   // 每队列一个 cv
    std::vector<std::thread> send_threads_;          // 发送线程组
    std::mutex mutex_;                     // 一把锁保护 slaves_ + 所有 pending_
    bool stop_ = false;
};

#endif
