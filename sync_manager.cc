#include "sync_manager.h"
#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>
#include "log.h"

SyncManager::~SyncManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    for (auto& cv : cvs_) cv->notify_all();
    for (auto& t : send_threads_) {
        if (t.joinable()) t.join();
    }
}

// 启动 n 个发送线程（由 main() 在读取配置后调用）
void SyncManager::startSenders(int n) {
    if (n <= 0) n = 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!send_threads_.empty()) return;   // 已启动
    pending_.resize(n);
    cvs_.resize(n);
    for (int i = 0; i < n; i++) {
        cvs_[i] = std::make_unique<std::condition_variable>();
        send_threads_.emplace_back(&SyncManager::sendLoop, this, i);
    }
}

void SyncManager::addSlave(int fd){
    std::lock_guard<std::mutex> lock(mutex_);
    // 保持阻塞模式：sendLoop 在锁外阻塞 write，一次 write 把完整命令写入
    // TCP 发送队列（阻塞 socket 语义），eBPF 捕获到的一定是完整命令；
    // 非阻塞 + 部分写会让 eBPF 捕获半个命令 → 从库解析错乱卡死，
    // 而 EAGAIN 误判 removeSlave 会静默丢数据。
    // 阻塞只卡住该 sendLoop 线程（队列自然积压=背压），不持广播锁、不影响 worker。
    SlaveInfo info;
    info.fd = fd;
    info.full_sync_done = false;
    slaves_.push_back(info);
}

void SyncManager::removeSlave(int fd){
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = slaves_.begin(); it != slaves_.end(); ) {
        if (it->fd == fd) {
            it = slaves_.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncManager::markFullSyncDone(int fd){
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slave : slaves_) {
        if (slave.fd == fd) {
            slave.full_sync_done = true;
            return;
        }
    }
}

// worker 热路径：按 key 哈希分片入队，不写网络。
// 临界区 = 判空 + 哈希 + push_back（微秒级）；write 由 sendLoop 在锁外执行。
// 无从库时直接返回（无同步场景零开销，不影响基线 QPS）。
// 同 key 永远哈希到同一队列 → 同发送线程 → 顺序保持。
void SyncManager::broadcast(const std::string& cmd, const std::string& key){
    std::lock_guard<std::mutex> lock(mutex_);
    if (slaves_.empty()) return;
    int n = (int)pending_.size();
    if (n == 0) return;                        // 发送线程未启动（理论上不会：addSlave 前必已 startSenders）
    size_t h = std::hash<std::string>{}(key);
    int idx = (int)(h % (size_t)n);
    pending_[idx].push_back(cmd);
    cvs_[idx]->notify_one();
}

// 第 idx 个发送线程：从自己的队列出队 → 锁外逐条 write。
// eBPF 在 write 处触发，但已不在 worker 热路径、不持广播锁。
// 必须逐条 write：eBPF 程序固定取第一条命令，批量 write 会丢数据。
void SyncManager::sendLoop(int idx){
    while (true) {
        std::string cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cvs_[idx]->wait(lock, [this, idx] {
                return !pending_[idx].empty() || stop_;
            });
            if (stop_ && pending_[idx].empty()) break;
            cmd = std::move(pending_[idx].front());
            pending_[idx].pop_front();
        }   // 解锁：write 在锁外

        // 锁内快照需要广播的从库 fd（full_sync_done 门控）
        std::vector<int> fds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& s : slaves_) {
                if (s.full_sync_done) fds.push_back(s.fd);
            }
        }
        for (int fd : fds) {
            // ★ eBPF 侧固定读 256B（verifier 要求常量 size）：若命令字符串的堆块
            //   不足 256B，越界部分可能落入未映射页 → bpf_probe_read_user 返回
            //   -EFAULT → 该事件数据残缺被丢弃（实测 300 条丢 4 条）。
            //   这里先保证缓冲区容量 ≥ 256B，使 eBPF 的 256B 读取始终落在已分配内存内。
            if (cmd.capacity() < 256) cmd.reserve(256);

            // 阻塞 fd 完整写出（处理 EINTR/部分写），确保 eBPF 捕获完整命令
            size_t off = 0;
            bool failed = false;
            while (off < cmd.size()) {
                ssize_t n = write(fd, cmd.data() + off, cmd.size() - off);
                if (n > 0) { off += (size_t)n; continue; }
                if (n < 0 && errno == EINTR) continue;
                // 连接已断（EPIPE/ECONNRESET 等）：移除，等 slave 重连后重新全量
                failed = true;
                break;
            }
            if (failed) {
                LOG_WARN("SyncManager: async write to slave fd=%d failed, errno=%d, removing slave", fd, errno);
                // 只移除不 close：fd 归 ntyco 协程管理（client_handler recv 到断开后 nty_close），
                // 裸 close 会与 nty_close 双重关闭并破坏 epoll 状态（曾导致死锁）
                removeSlave(fd);
            }
        }
    }
}
