#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <csignal>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <ctime>
#include "hashtable.h"
#include "rbtree.h"
#include "skiplist.h"
#include "persist.h"
#include "BusinessHandler.h"
#include "rdma_sync.h"
#include "ntynet.h"          // 阶段7-3：SYNC_FINISHED_CMD 常量 + startServer
#include "config.h"          // 日志级别配置（kvstore.conf 的 loglevel）
#include "log.h"             // 日志宏（进度类打印受 loglevel 控制）

// ===== 阶段7-3：改为独立运行标志。master 断开只停广播兜底，不退出 slave =====
static std::atomic<bool> g_running(true);
static BusinessHandler* g_handler = nullptr;

// 验证指定范围的 key；miss 时按"单点/连续区间"合并打印，便于定位增量同步丢失形态
static void verifyKeys(Engine* engine, int start, int end, const std::string& label) {
    int found = 0, miss = 0;
    std::vector<int> missKeys;   // 记录 miss 的 key 序号（升序）
    for (int i = start; i <= end; i++) {
        std::string key = "key" + std::to_string(i);
        std::string expected = "value" + std::to_string(i);
        std::string value;
        if (engine->get(key, value) && value == expected) {
            found++;
        } else {
            miss++;
            missKeys.push_back(i);
        }
    }
    std::cout << "  " << label << " (key" << start << "~key" << end << "): found=" << found << " miss=" << miss << std::endl;

    if (!missKeys.empty()) {
        // 把 miss 的 key 合并成连续区间：单点打印 keyN，连续段打印 keyN---keyM
        std::cout << "    missing keys: ";
        size_t i = 0;
        bool first = true;
        while (i < missKeys.size()) {
            int segStart = missKeys[i];
            int segEnd = segStart;
            while (i + 1 < missKeys.size() && missKeys[i + 1] == segEnd + 1) {
                i++;
                segEnd = missKeys[i];
            }
            if (!first) std::cout << ", ";
            first = false;
            if (segStart == segEnd)
                std::cout << "key" << segStart;
            else
                std::cout << "key" << segStart << "---key" << segEnd;
            i++;
        }
        std::cout << std::endl;
    }
}

// 解析 RESP 回复并打印
static void printRespReply(const std::string& reply) {
    if (reply.empty()) {
        std::cout << "(empty)" << std::endl;
        return;
    }
    if (reply[0] == '+') {
        size_t end = reply.find("\r\n");
        if (end != std::string::npos)
            std::cout << reply.substr(1, end - 1) << std::endl;
    } else if (reply[0] == '-') {
        size_t end = reply.find("\r\n");
        if (end != std::string::npos)
            std::cout << "(error) " << reply.substr(1, end - 1) << std::endl;
    } else if (reply[0] == '$') {
        if (reply[1] == '-') {
            std::cout << "(nil)" << std::endl;
        } else {
            size_t pos = reply.find("\r\n");
            if (pos != std::string::npos) {
                std::string value = reply.substr(pos + 2);
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
                    value.pop_back();
                std::cout << "\"" << value << "\"" << std::endl;
            }
        }
    } else if (reply[0] == ':') {
        size_t end = reply.find("\r\n");
        if (end != std::string::npos)
            std::cout << "(integer) " << reply.substr(1, end - 1) << std::endl;
    } else if (reply[0] == '*') {
        size_t pos = reply.find("\r\n");
        if (pos != std::string::npos) {
            int count = std::stoi(reply.substr(1, pos - 1));
            std::cout << count << " items:" << std::endl;
            std::string remaining = reply.substr(pos + 2);
            for (int i = 0; i < count; i++) {
                if (remaining.empty()) break;
                if (remaining[0] == '$') {
                    if (remaining[1] == '-') {
                        std::cout << "  " << (i + 1) << ") (nil)" << std::endl;
                        size_t p = remaining.find("\r\n");
                        if (p != std::string::npos)
                            remaining = remaining.substr(p + 2);
                    } else {
                        size_t p = remaining.find("\r\n");
                        if (p != std::string::npos) {
                            int len = std::stoi(remaining.substr(1, p - 1));
                            remaining = remaining.substr(p + 2);
                            std::string val = remaining.substr(0, len);
                            std::cout << "  " << (i + 1) << ") \"" << val << "\"" << std::endl;
                            remaining = remaining.substr(len + 2);
                        }
                    }
                }
            }
        }
    } else {
        std::cout << reply << std::endl;
    }
}

// 后台线程：接收主库推送的增量数据（广播兜底通道）
static void receiveThread(int fd, BusinessHandler* handler, std::atomic<int>& cmdCount) {
    (void)handler;
    std::string readBuf;
    char buf[65536];
    while (g_running) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            // 阶段7-7：master 断开只停兜底通道，不再影响 slave 主体（2000 统一端口继续）
            LOG_WARN("slave: master sync connection closed (broadcast fallback stopped)");
            break;
        }
        readBuf.append(buf, n);

        bool progress = true;
        while (progress) {
            progress = false;
            std::string cmd;
            std::vector<std::string> args;
            size_t consumed = 0;
            if (BusinessHandler::parseRespCommand(readBuf, cmd, args, consumed)) {
                // 阶段7-7：2000 广播仅作 eBPF 数据源，slave 读丢弃，增量统一走 2000 eBPF 通道
                readBuf.erase(0, consumed);
                cmdCount++;
                if (cmdCount % 10000 == 0) {
                    LOG_INFO("slave: processed %d incremental commands", cmdCount.load());
                }
                progress = true;
            } else {
                bool skipped = false;
                while (!readBuf.empty() && readBuf[0] != '*') {
                    readBuf.erase(0, 1);
                    skipped = true;
                }
                if (skipped) {
                    progress = true;   // 清掉了非 RESP 垃圾，重新尝试解析
                }
                // 以 '*' 开头但解析不完整：等更多数据（progress 保持 false，避免死循环）
            }
        }
    }
}

// （阶段7-7 端口统一：原 ebpfReceiverThread(2002) 已移除，
//   eBPF 转发数据改由 2000 统一 RESP 服务器接收）

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    // 0. 读取配置文件（如果存在），控制日志输出级别：
    //    loglevel = debug/info/warn/error；设 warn 或 error 可得到干净终端
    //    （进度类打印走 LOG_* 到 stderr；用户主动命令结果走 stdout 不受影响）
    Config config;
    config.load("kvstore.conf");
    std::string logLevel = config.getString("loglevel", "info");
    if (logLevel == "debug") g_logLevel = LOG_LEVEL_DEBUG;
    else if (logLevel == "warn") g_logLevel = LOG_LEVEL_WARN;
    else if (logLevel == "error") g_logLevel = LOG_LEVEL_ERROR;
    else g_logLevel = LOG_LEVEL_INFO;

    // 1. 从命令行读 master IP（跨机：./kvslave 192.168.1.132；本机测试不传默认 127.0.0.1）
    const char* master_ip = "127.0.0.1";
    if (argc > 1) master_ip = argv[1];
    // 2. 连接主库客户端端口（2000）
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2000);
    inet_pton(AF_INET, master_ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("slave: connect to master %s failed", master_ip);
        return 1;
    }
    LOG_INFO("slave: connected to master %s, sending SYNC...", master_ip);

    // 3. 发送 SYNC 命令
    std::string syncCmd = "*1\r\n$4\r\nSYNC\r\n";
    ssize_t unused = write(fd, syncCmd.data(), syncCmd.size());
    (void)unused;

    // 4. 阶段7-3：启动 RDMA server（监听 2003，可被多次 recv_file 复用）
    if (rdma_server_start(RDMA_SYNC_PORT) != 0) {
        LOG_ERROR("slave: rdma_server_start failed");
        close(fd);
        return 1;
    }
    LOG_INFO("slave: rdma_server_start: listening on port %d", RDMA_SYNC_PORT);

    // 5. 阶段7-3：接收 master 发来的全量 .tmp 文件，落盘为 dump.rdb
    //    路径放本地 /tmp：工作目录是 hgfs 共享（fuse），85MB 读写慢 ~200ms+；
    //    本地盘读写 ~5 倍。文件只是加载中间态，重启后重新全量生成。
    LOG_INFO("slave: waiting for full sync file on port %d ...", RDMA_SYNC_PORT);
    // 全量同步进度打印已按要求关闭（如需排查耗时，恢复下面的 st0/st1 计时与 fprintf）
    int recvLen = rdma_server_recv_file(RDMA_SYNC_PORT, "/tmp/kvslave_dump.rdb");
    if (recvLen <= 0) {
        LOG_ERROR("slave: rdma_server_recv_file failed");
        close(fd);
        return 1;
    }
    LOG_INFO("slave: full sync data received, size=%d", recvLen);

    // 5. 创建引擎 + Persist（阶段7-3：RDB 模式，只读加载快照，不起 AOF flush 线程）
    auto hashtable = std::make_unique<HashTable>();
    auto rbtree = std::make_unique<RBTree>();
    auto skiplist = std::make_unique<SkipList>();

    Persist persist("/tmp/kvslave_dump.aof", hashtable.get(), PERSIST_MODE_RDB);
    persist.loadSnapshot();

    // 6. 创建 BusinessHandler
    BusinessHandler handler(hashtable.get(), rbtree.get(), skiplist.get(),
                            nullptr, nullptr, nullptr);
    g_handler = &handler;

    // 7. 验证全量同步数据
    int foundAfterFullSync = 0;
    for (int i = 1; i <= 50000; i++) {
        std::string key = "key" + std::to_string(i);
        std::string expected = "value" + std::to_string(i);
        std::string value;
        if (hashtable->get(key, value) && value == expected) {
            foundAfterFullSync++;
        }
    }
    LOG_INFO("slave: full sync verified: %d/50000 keys OK", foundAfterFullSync);

    // 8. 阶段7-3：通知 master 全量同步完成（RESP 格式，命令名/长度与 master 共享常量）
    std::string finishCmd = SYNC_FINISHED_CMD;
    ssize_t nfin = write(fd, finishCmd.data(), finishCmd.size());
    if (nfin > 0) {
        LOG_INFO("slave: sync_finished sent to master");
    } else {
        LOG_WARN("slave: WARN sync_finished write failed");
    }

    LOG_INFO("slave: full sync done, waiting for incremental updates...");
    LOG_INFO("slave: you can type get <key>, mget <key1> <key2> ..., verify_1~verify_10, verify10w, or cmdcount");

    // 9. 启动后台线程接收增量（广播兜底，2000 出站连接）
    std::atomic<int> cmdCount(0);
    std::thread recvThread(receiveThread, fd, &handler, std::ref(cmdCount));
    recvThread.detach();

    // 10. 端口统一（阶段7-7）：slave 监听 2000，统一 RESP 服务
    //     - 主库 eBPF 转发连接（写命令 SET/DEL，增量主力通道）
    //     - 客户端只读查询（GET 等）
    //     协议均为 RESP，由同一服务器处理；RDMA 全量保留 2003 端口
    //     多 worker（slave_workers）：引擎为分片锁线程安全，
    //     单 worker 时增量消化约 2 万 cmd/s，追不上主库写入（6+3 同步 7~8 万/s）；
    //     实测 2 worker 可把追赶吞吐提升约一倍。
    int slaveWorkers = config.getInt("slave_workers", 2);
    LOG_INFO("slave: starting unified RESP server on port 2000 (%d workers)", slaveWorkers);
    std::thread serverThread([&handler, slaveWorkers]() {
        startServer(&handler, 2000, slaveWorkers, false);
    });
    serverThread.detach();

    // 12. 主线程处理用户输入
    while (g_running) {
        std::string line;
        if (!std::getline(std::cin, line)) {
            // stdin 关闭（nohup/后台运行）：转为守护模式，绝不退出，
            // 否则 main 返回时 recvThread(joinable) 析构会 std::terminate，
            // 且 detached 的 2000 服务器线程也会被 exit() 一并终止。
            LOG_WARN("slave: stdin closed, entering background mode");
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            break;
        }
        if (line.empty()) continue;

        if (line.empty()) continue;

        if (line.substr(0, 7) == "verify_") {
            std::string numStr = line.substr(7);
            int group = std::stoi(numStr);
            if (group >= 1 && group <= 10) {
                int start = (group - 1) * 10000 + 1;
                int end = group * 10000;
                std::string label = "Group " + std::to_string(group);
                verifyKeys(hashtable.get(), start, end, label);
            } else {
                std::cout << "slave: invalid group number, use 1~10" << std::endl;
            }
        } else if (line == "verify10w") {
            std::cout << "slave: verifying all 100000 keys..." << std::endl;
            for (int g = 1; g <= 10; g++) {
                int start = (g - 1) * 10000 + 1;
                int end = g * 10000;
                std::string label = "Group " + std::to_string(g);
                verifyKeys(hashtable.get(), start, end, label);
            }
        } else if (line == "cmdcount") {
            std::cout << "slave: total incremental commands processed: " << cmdCount << std::endl;
        } else if (line.substr(0, 4) == "get " || line == "get") {
            std::string reply;
            handler.handleCommand(line, reply);
            std::cout << "slave: ";
            printRespReply(reply);
        } else if (line.substr(0, 5) == "mget " || line == "mget") {
            std::string reply;
            handler.handleCommand(line, reply);
            std::cout << "slave: ";
            printRespReply(reply);
        } else {
            std::cout << "slave: unknown command, try get <key>, mget <key1> <key2> ..., verify_1~verify_10, verify10w, or cmdcount" << std::endl;
        }
    }

    close(fd);
    return 0;
}

