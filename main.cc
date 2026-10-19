#include <iostream>
#include <memory>
#include <vector>
#include <cstring>
#include <thread>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include "hashtable.h"
#include "rbtree.h"
#include "skiplist.h"
#include "config.h"
#include "persist.h"
#include "BusinessHandler.h"
#include "sync_manager.h"
#include "ntynet.h"
#include "log.h"

SyncManager g_syncManager;

int main() {
    signal(SIGPIPE, SIG_IGN);
    Config config;
    config.load("kvstore.conf");
    int port = config.getInt("port", 2000);
    std::string logLevel = config.getString("loglevel", "info");
    if (logLevel == "debug") g_logLevel = LOG_LEVEL_DEBUG;
    else if (logLevel == "warn") g_logLevel = LOG_LEVEL_WARN;
    else if (logLevel == "error") g_logLevel = LOG_LEVEL_ERROR;
    else g_logLevel = LOG_LEVEL_INFO;

    std::string persistMode = config.getString("persist_mode", "both");
    PersistMode pm;
    if (persistMode == "rdb") pm = PERSIST_MODE_RDB;
    else if (persistMode == "aof") pm = PERSIST_MODE_AOF;
    else pm = PERSIST_MODE_BOTH;
    LOG_INFO("persist mode: %s", persistMode.c_str());

    // 读取 RDMA 配置（eBPF 配置已由独立进程 ebpf_proxy 通过命令行参数接收）
    std::string rdmaSlaveIp = config.getString("rdma_slave_ip", "127.0.0.1");
    int rdmaSlavePort = config.getInt("rdma_slave_port", 2003);

    // 读取 worker 数量（多线程核心配置）
    const int WORKER_COUNT = config.getInt("workers", 1);

    // 读取广播发送线程数（key 分片并行发送，突破单线程 write+eBPF 瓶颈）
    int syncSenders = config.getInt("sync_senders", 1);
    g_syncManager.startSenders(syncSenders);

    // ===== 多线程改造：只创建一套引擎，所有 worker 共享 =====
    auto hashtable = std::make_unique<HashTable>();
    auto rbtree = std::make_unique<RBTree>();
    auto skiplist = std::make_unique<SkipList>();
    auto hPersist = std::make_unique<Persist>("server_0_h.aof", hashtable.get(), pm);
    auto rPersist = std::make_unique<Persist>("server_0_r.aof", rbtree.get(), pm);
    auto sPersist = std::make_unique<Persist>("server_0_s.aof", skiplist.get(), pm);

    auto handler = std::make_unique<BusinessHandler>(
        hashtable.get(), rbtree.get(), skiplist.get(),
        hPersist.get(), rPersist.get(), sPersist.get()
    );
    handler->setSyncManager(&g_syncManager);

    // 恢复数据
    hPersist->recover();
    rPersist->recover();
    sPersist->recover();

    // 设置 RDMA 配置
    setRdmaConfig(rdmaSlaveIp.c_str(), rdmaSlavePort);

    // 阶段7-7：eBPF 已独立为 ebpf_proxy 进程（sudo ./ebpf_proxy <kvserver_pid> <从库IP> 2000），
    // kvserver 不再内嵌 eBPF（无需 root 即可运行）

    // 主从同步线程（保持不变，监听 2001 端口）
    std::thread syncThread([]() {
        int syncFd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(syncFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(2001);
        bind(syncFd, (struct sockaddr*)&addr, sizeof(addr));
        listen(syncFd, 5);
        while (true) {
            int slaveFd = accept(syncFd, nullptr, nullptr);
            if (slaveFd != -1) {
                g_syncManager.addSlave(slaveFd);
            }
        }
    });
    syncThread.detach();

    // 启动多线程 ntyco 网络服务（阻塞，不会返回）
    startNtycoServer(handler.get(), port, WORKER_COUNT);

    return 0;
}