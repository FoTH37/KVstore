#define NTY_CO_MAX_STACKSIZE (2 * 1024 * 1024)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-macro-redefined"
#pragma GCC diagnostic ignored "-Wpointer-arith"
extern "C" {
#include "nty_coroutine.h"
}
#pragma GCC diagnostic pop

#include "ntynet.h"
#include "sync_manager.h"
#include "rdma_sync.h"
#include "log.h"
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cctype>
#include <ctime>

static BusinessHandler* g_handler = nullptr;

// RDMA 配置
static const char* g_rdma_slave_ip = "127.0.0.1";
static int g_rdma_slave_port = RDMA_SYNC_PORT;

void setRdmaConfig(const char* slave_ip, int slave_port) {
    g_rdma_slave_ip = slave_ip;
    g_rdma_slave_port = slave_port;
}

// 全量同步：SAVE → 生成 .tmp → rdma-client 发送（阶段7-4）
static void handleFullSync(int client_fd) {
    // 阶段7-6：恢复登记 SyncManager —— broadcast 的 write() 是 eBPF 增量同步的数据源
    SyncManager* sm = g_handler->getSyncManager();
    if (sm) {
        sm->addSlave(client_fd);
        sm->markFullSyncDone(client_fd);
    }
    // 全量同步进度打印已按要求关闭（如需排查耗时，恢复下面注释掉的 hfs_t0/hfs_t1 计时与 fprintf）
    auto hfs_elapsed = [&](const char* tag) {
        (void)tag;
    };
    // 1. 触发 SAVE 命令，生成 RDB 快照（rdb 模式：只生成 rdb，不写 aof）
    //    saveSnapshot 内部已是"写 .tmp + rename"原子发布，文件就绪即完整
    std::string saveCmd = "*1\r\n$4\r\nSAVE\r\n";
    std::string reply;
    g_handler->handleCommand(saveCmd, reply);
    hfs_elapsed("SAVE(snapshot)");

    // 2. 直接传输 server_0_h.rdb（不再复制 .tmp——原 copy 在 hgfs 共享目录
    //    85MB 需 ~1.6s 纯属浪费；open 后 inode 固定，传输期间即使有新 SAVE
    //    rename 覆盖也不影响已打开的 fd）
    std::string src = "server_0_h.rdb";
    hfs_elapsed("(skip copy)");

    // 3. rdma-client 发送（阻塞直到传输完成，RDMA 资源内部自动销毁）
    int ret = rdma_client_send_file(g_rdma_slave_ip, g_rdma_slave_port, src.c_str());
    hfs_elapsed("rdma_client_send_file");
    if (ret != 0) {
        LOG_ERROR("handleFullSync: rdma send failed");
        // 修复：失败路径必须注销该 slave，否则广播 write() 会阻塞持锁拖死全部 worker
        if (sm) {
            sm->removeSlave(client_fd);
        }
        return;
    }
    LOG_INFO("handleFullSync: rdb sent to slave, waiting sync_finished");
    // 4. 不在此等待；连接保持，slave 加载完成后会通过同一连接发 SYNC_FINISHED
    //    收到后由 onSyncFinished() 清理（原删除 .tmp 已无对象，仅保留钩子语义）
}

// 全量同步完成回调（阶段7-4）
// 阶段7-7：eBPF 已独立为 ebpf_proxy 进程，无需再通知内嵌 eBPF 切换转发
// 2026-09：handleFullSync 直接传输 server_0_h.rdb（saveSnapshot 原子发布），
// 不再生成传输用 .tmp，此处无清理对象，仅保留日志钩子
static void onSyncFinished() {
    LOG_INFO("onSyncFinished: full sync done, slave ready for incremental");
}


// ===== 阶段7-2：连接参数（fd + 只读标志），供 master/slave 复用 =====
struct ConnArgs {
    int fd;
    bool read_only;
};

// 只读模式（从库 2000 统一端口）下被拦截的写命令
static bool isReadOnlyBlocked(const std::string& cmd) {
    static const char* blocked[] = {"SET","MSET","DEL","HSET","RSET","SSET",
                                    "HDEL","RDEL","SDEL","SAVE","SYNC"};
    for (auto* b : blocked) {
        if (cmd == b) return true;
    }
    return false;
}

// 发送回复的辅助函数
static void sendReply(int fd, const std::string& reply) {
    size_t sent = 0;
    while (sent < reply.size()) {
        int n = nty_send(fd, reply.data() + sent, reply.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

// 每个客户端连接的协程处理函数
static void client_handler(void *arg) {
    ConnArgs* ca = (ConnArgs*)arg;
    int fd = ca->fd;
    bool read_only = ca->read_only;
    free(arg);
    // 修改1：256KB 接收缓冲区改用堆分配（std::vector），
    // 避免大栈帧在 ntyco ucontext 共享栈上触发 -fstack-clash-protection 探针崩溃
    std::vector<char> buf(256 * 1024);
    std::string readBuf;
    readBuf.reserve(256 * 1024);   // 预分配：避免 append 反复扩容（malloc+拷贝+free）
    // 批量回复缓冲：累积一批命令的回复再统一发送，减少 nty_send 次数
    std::string replyBuf;
    replyBuf.reserve(8192);        // 预分配：避免 replyBuf += reply 反复扩容（8KB 阈值内纯内存写入）
    auto flushReply = [&]() {
        if (!replyBuf.empty()) {
            sendReply(fd, replyBuf);
            replyBuf.clear();
        }
    };
    while (1) {
        int ret = nty_recv(fd, buf.data(), buf.size() - 1, 0);
        if (ret <= 0) {
            nty_close(fd);
            break;
        }
        buf[ret] = '\0';
        readBuf.append(buf.data(), ret);
        // 修改2：用游标 pos 代替每命令一次 readBuf.erase(0, consumed)，
        // 整批处理完只做一次 erase(0, pos)，避免 O(n²) 的反复搬移
        size_t pos = 0;
        bool progress = true;
        while (progress) {
            progress = false;
            std::string cmd;
            std::vector<std::string> args;
            size_t consumed = 0;
            if (BusinessHandler::parseRespCommandAt(readBuf, pos, cmd, args, consumed)) {
                if (cmd == "SYNC" && !read_only) {
                    flushReply();
                    handleFullSync(fd);
                    pos = consumed;
                    continue;   // 不 return：保持连接等 slave 的 sync_finished
                }
                if (cmd == SYNC_FINISHED_NAME && !read_only) {
                    LOG_INFO("client_handler: received sync_finished from slave");
                    flushReply();
                    onSyncFinished();
                    pos = consumed;
                    continue;
                }
                if (read_only && isReadOnlyBlocked(cmd)) {
                    replyBuf += "-ERR read-only slave\r\n";
                    pos = consumed;
                    progress = true;
                    continue;
                }
                // 修改3：解析只做一次，直接执行已解析的命令，
                // 不再 handleCommand(readBuf.substr(0, consumed)) 二次解析
                std::string reply;
                g_handler->handleParsedCommand(cmd, args, reply);
                replyBuf += reply;
                pos = consumed;
                // 回复累积到 8KB 再统一发送（批尾也会 flush）
                if (replyBuf.size() >= 8192) flushReply();
                progress = true;
            } else {
                bool skipped = false;
                while (pos < readBuf.size() && (readBuf[pos] == '\r' || readBuf[pos] == '\n')) {
                    pos++;
                    skipped = true;
                }
                if (skipped && pos < readBuf.size() && readBuf[pos] == '*') {
                    // 跳过残留换行后是 RESP，继续尝试解析
                    progress = true;
                } else if (pos < readBuf.size() && readBuf[pos] != '*') {
                    // 非 RESP 格式（文本协议），直接处理
                    // 只读模式：提取首词拦截写命令
                    if (read_only) {
                        std::string first(readBuf.data() + pos, readBuf.size() - pos);
                        size_t p = first.find_first_not_of(" \t\r\n");
                        if (p != std::string::npos) {
                            first = first.substr(p);
                            size_t e = first.find_first_of(" \t\r\n\"");
                            std::string tok = first.substr(0, e);
                            for (auto& c : tok) c = toupper((unsigned char)c);
                            if (isReadOnlyBlocked(tok)) {
                                replyBuf += "-ERR read-only slave\r\n";
                                pos = readBuf.size();
                                progress = true;
                                continue;
                            }
                        }
                    }
                    std::string reply;
                    g_handler->handleCommand(std::string(readBuf.data() + pos, readBuf.size() - pos), reply);
                    replyBuf += reply;
                    pos = readBuf.size();   // 与原来 readBuf.clear() 等价：整块消费
                    progress = true;
                } else if (pos < readBuf.size() && readBuf[pos] == '*') {
                    // 修改4：以 * 开头但解析失败
                    // 可能是不完整的命令（批量太大，一次 recv 没读全）
                    // 也可能损坏的恶意数据
                    // 正确做法：不清空、不关闭连接，等更多数据（progress 保持 false）
                    // 回到外层 while(1) 继续 recv，读到剩余数据后再解析
                }
            }
        }
        // 批处理结束：一次性清除已消费部分，并发送剩余回复
        if (pos > 0) readBuf.erase(0, pos);
        flushReply();
    }
}

// ===== 多线程改造：server 协程的参数结构 =====
struct ServerArgs {
    int port;
    int worker_id;
    bool read_only;
};

// server 协程：每个 worker 线程一个，独立 accept
static void server(void *arg) {
    ServerArgs* sa = (ServerArgs*)arg;
    int port = sa->port;
    int worker_id = sa->worker_id;

    int fd = nty_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("worker %d: ntyco socket failed", worker_id);
        return;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    // 关键：多线程共享同一端口，内核负载均衡分配连接
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        LOG_ERROR("worker %d: ntyco bind failed on port %d", worker_id, port);
        return;
    }

    if (listen(fd, 128) < 0) {
        LOG_ERROR("worker %d: ntyco listen failed", worker_id);
        return;
    }

    LOG_INFO("worker %d: ntyco listening on port %d", worker_id, port);

    while (1) {
        struct sockaddr_in remote;
        socklen_t len = sizeof(remote);
        int cli_fd = nty_accept(fd, (struct sockaddr*)&remote, &len);
        if (cli_fd < 0) continue;

        ConnArgs* ca = (ConnArgs*)malloc(sizeof(ConnArgs));
        ca->fd = cli_fd;
        ca->read_only = sa->read_only;

        nty_coroutine *read_co;
        nty_coroutine_create(&read_co, client_handler, ca);
    }
}

// 先声明 nty_schedule_create（nty_coroutine.c 中定义，头文件未声明）
extern "C" int nty_schedule_create(int stack_size);

// 每个 worker 线程的入口：独立调度器 + server 协程
static void worker_thread(int port, int worker_id, bool read_only) {
    // 每个线程创建独立的调度器（ntyco 调度器是线程绑定的）
    nty_schedule_create(4 * 1024 * 1024);

    ServerArgs sa;
    sa.port = port;
    sa.worker_id = worker_id;
    sa.read_only = read_only;

    nty_coroutine *co = NULL;
    nty_coroutine_create(&co, server, &sa);
    nty_schedule_run();  // 阻塞，直到调度器完成
}

// ===== 阶段7-2：通用 server 入口（master 完整读写 / slave 只读 GET）=====
void startServer(BusinessHandler* handler, int port, int worker_count, bool read_only) {
    g_handler = handler;

    if (worker_count < 1) worker_count = 1;

    std::vector<std::thread> threads;
    for (int i = 0; i < worker_count; i++) {
        threads.emplace_back(worker_thread, port, i, read_only);
    }
    // 主线程阻塞等待所有 worker（它们不会退出，因为 server 协程是无限循环）
    for (auto& t : threads) {
        t.join();
    }
}

// 兼容包装：master 完整读写模式
void startNtycoServer(BusinessHandler* handler, int port, int worker_count) {
    startServer(handler, port, worker_count, false);
}
