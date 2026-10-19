// slave_verify5w.cc —— 从库外部验证工具（前 5 万条）
// 对从库统一 RESP 端口（默认 2000）做 5 万次 GET，验证 key1~key50000 数据一致性
// 与 slave_verify10w.cc 完全一致，仅 TOTAL_KEYS 不同
//
// 用法：
//   ./slave_verify5w                 # 连 127.0.0.1:2000（从库本机）
//   ./slave_verify5w 192.168.1.44    # 连指定从库 IP 的 2000 端口
//   ./slave_verify5w 192.168.1.44 2000   # 指定 IP 和端口
//
// 输出（脚本可 grep "Status: PASS"）：
//   Found / Miss / Errors / Time / QPS / Status

#include <stdio.h>
#include <string.h>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>

const int DEFAULT_PORT = 2000;   // 从库统一 RESP 端口（查询 + eBPF 转发接收）
const int TOTAL_KEYS = 50000;    // 只验证前 5 万条（key1~key50000）
const int NUM_THREADS = 16;
const int BATCH_SIZE = 1000;

std::atomic<int> g_found(0);
std::atomic<int> g_miss(0);
std::atomic<int> g_errors(0);

static const char* g_server_ip = "127.0.0.1";
static int g_server_port = DEFAULT_PORT;

std::string buildGetCmd(const std::string& key) {
    return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
}

int connectServer() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_server_port);
    if (inet_pton(AF_INET, g_server_ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 每个线程负责一部分 key（长连接 + pipeline）
void worker(int thread_id, int start_key, int end_key) {
    int fd = connectServer();
    if (fd < 0) {
        printf("FAIL: thread %d connect failed (server %s:%d)\n",
               thread_id, g_server_ip, g_server_port);
        return;
    }

    std::vector<std::string> batch;
    std::vector<int> batchKeys;
    batch.reserve(BATCH_SIZE);

    for (int i = start_key; i <= end_key; i++) {
        std::string key = "key" + std::to_string(i);
        batch.push_back(buildGetCmd(key));
        batchKeys.push_back(i);

        if ((int)batch.size() == BATCH_SIZE || i == end_key) {
            std::string data;
            for (const auto& cmd : batch) data += cmd;

            ssize_t n = write(fd, data.data(), data.size());
            if (n != (ssize_t)data.size()) {
                g_errors += (int)batch.size();
                batch.clear();
                batchKeys.clear();
                continue;
            }

            char buf[65536];
            std::string recvBuf;
            int okCount = 0;
            while (okCount < (int)batch.size()) {
                int r = read(fd, buf, sizeof(buf));
                if (r <= 0) break;
                recvBuf.append(buf, r);

                while (okCount < (int)batch.size()) {
                    if (recvBuf.empty()) break;
                    if (recvBuf[0] != '$') break;

                    if (recvBuf[1] == '-') {
                        if (recvBuf.size() < 5) break;
                        if (recvBuf.substr(0, 5) == "$-1\r\n") {
                            g_miss++;
                            recvBuf.erase(0, 5);
                            okCount++;
                            continue;
                        }
                        break;
                    }

                    size_t firstNl = recvBuf.find('\n');
                    if (firstNl == std::string::npos) break;
                    int len = atoi(recvBuf.substr(1, firstNl - 1).c_str());
                    size_t headerLen = firstNl + 1;
                    if (recvBuf.size() < headerLen + len + 2) break;

                    std::string value = recvBuf.substr(headerLen, len);
                    recvBuf.erase(0, headerLen + len + 2);

                    int keyIdx = batchKeys[okCount];
                    std::string expected = "value" + std::to_string(keyIdx);
                    if (value == expected) {
                        g_found++;
                    } else {
                        g_errors++;
                    }
                    okCount++;
                }
            }

            batch.clear();
            batchKeys.clear();
        }
    }

    close(fd);
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) g_server_ip = argv[1];
    if (argc > 2) g_server_port = atoi(argv[2]);

    printf("Verifying %d keys on slave %s:%d (fast: %d threads, batch %d)...\n",
           TOTAL_KEYS, g_server_ip, g_server_port, NUM_THREADS, BATCH_SIZE);

    auto start = std::chrono::steady_clock::now();

    int keysPerThread = TOTAL_KEYS / NUM_THREADS;
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        int startKey = t * keysPerThread + 1;
        int endKey = (t == NUM_THREADS - 1) ? TOTAL_KEYS : (t + 1) * keysPerThread;
        threads.emplace_back(worker, t, startKey, endKey);
    }
    for (auto& th : threads) {
        th.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    printf("\nResults:\n");
    printf("  Found:  %d\n", g_found.load());
    printf("  Miss:   %d\n", g_miss.load());
    printf("  Errors: %d\n", g_errors.load());
    printf("Time: %ld ms, QPS: %.0f\n", (long)ms, (double)TOTAL_KEYS / (ms / 1000.0));
    printf("Status: %s\n", (g_miss.load() == 0 && g_errors.load() == 0) ? "PASS" : "FAIL");
    return (g_miss.load() == 0 && g_errors.load() == 0) ? 0 : 1;
}
