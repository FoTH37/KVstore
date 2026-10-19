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

const int PORT = 2000;
const int TOTAL_KEYS = 100000;
const int NUM_THREADS = 16;
const int BATCH_SIZE = 1000;

std::atomic<int> g_found(0);
std::atomic<int> g_miss(0);
std::atomic<int> g_errors(0);

std::string buildGetCmd(const std::string& key) {
    return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
}

int connectServer() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 解析 RESP 回复，返回 value 字符串
// 成功返回 value（不含 $N\r\n 和尾部 \r\n），key 不存在返回空字符串，协议错误返回 "(error)"
std::string parseRespValue(const std::string& reply) {
    if (reply.empty()) return "(error)";
    if (reply[0] == '$' && reply[1] == '-') return "";  // nil: $-1\r\n
    if (reply[0] == '$') {
        size_t pos = reply.find('\n');
        if (pos != std::string::npos) {
            // value 从 \n 之后开始，去掉末尾的 \r\n
            return reply.substr(pos + 1, reply.size() - pos - 3);
        }
    }
    return "(error)";
}

// 每个线程负责一部分 key（长连接 + pipeline）
void worker(int thread_id, int start_key, int end_key) {
    int fd = connectServer();
    if (fd < 0) {
        printf("FAIL: thread %d connect failed\n", thread_id);
        return;
    }

    std::vector<std::string> batch;
    std::vector<int> batchKeys;  // 记录每个 key 对应的编号
    batch.reserve(BATCH_SIZE);

    for (int i = start_key; i <= end_key; i++) {
        std::string key = "key" + std::to_string(i);
        batch.push_back(buildGetCmd(key));
        batchKeys.push_back(i);

        if ((int)batch.size() == BATCH_SIZE || i == end_key) {
            // 批量发送
            std::string data;
            for (const auto& cmd : batch) data += cmd;

            ssize_t n = write(fd, data.data(), data.size());
            if (n != (ssize_t)data.size()) {
                g_errors += (int)batch.size();
                batch.clear();
                batchKeys.clear();
                continue;
            }

            // 批量读回复并解析
            char buf[65536];
            std::string recvBuf;
            int okCount = 0;
            while (okCount < (int)batch.size()) {
                int r = read(fd, buf, sizeof(buf));
                if (r <= 0) break;
                recvBuf.append(buf, r);

                // 尝试从 recvBuf 解析出足够多的完整回复
                while (okCount < (int)batch.size()) {
                    // 解析一个 RESP bulk string 回复
                    if (recvBuf.empty()) break;
                    if (recvBuf[0] != '$') break;  // 不是 bulk string，等更多数据

                    if (recvBuf[1] == '-') {
                        // nil 回复: $-1\r\n
                        if (recvBuf.size() < 5) break;  // 数据不完整
                        if (recvBuf.substr(0, 5) == "$-1\r\n") {
                            g_miss++;                    // 删除未使用的 keyIdx
                            recvBuf.erase(0, 5);
                            okCount++;
                            continue;
                        }
                        break;
                    }

                    // $N\r\n
                    size_t firstNl = recvBuf.find('\n');
                    if (firstNl == std::string::npos) break;
                    int len = atoi(recvBuf.substr(1, firstNl - 1).c_str());
                    // 需要 $N\r\n + len字节 + \r\n
                    size_t headerLen = firstNl + 1;  // 跳过 \n
                    if (recvBuf.size() < headerLen + len + 2) break;  // 数据不完整

                    std::string value = recvBuf.substr(headerLen, len);
                    recvBuf.erase(0, headerLen + len + 2);  // 跳过 value + \r\n

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

int main() {
    signal(SIGPIPE, SIG_IGN);
    printf("Verifying %d keys (fast: %d threads, batch %d)...\n",
           TOTAL_KEYS, NUM_THREADS, BATCH_SIZE);

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