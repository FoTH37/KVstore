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
const int START_KEY = 50001;    // 起始 key
const int TOTAL_KEYS = 100000;  // 结束 key（包含）
const int NUM_THREADS = 16;
const int BATCH_SIZE = 1000;

std::atomic<int> g_failCount(0);

std::string buildSetCmd(const std::string& key, const std::string& value) {
    return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key
         + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
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

// 每个线程负责一部分 key（长连接 + pipeline 批量发送）
void worker(int thread_id, int start_key, int end_key) {
    int fd = connectServer();
    if (fd < 0) {
        printf("FAIL: thread %d connect failed\n", thread_id);
        return;
    }

    std::vector<std::string> batch;
    batch.reserve(BATCH_SIZE);

    for (int i = start_key; i <= end_key; i++) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        batch.push_back(buildSetCmd(key, value));

        if ((int)batch.size() == BATCH_SIZE || i == end_key) {
            // 批量发送
            std::string data;
            for (const auto& cmd : batch) data += cmd;

            ssize_t n = write(fd, data.data(), data.size());
            if (n != (ssize_t)data.size()) {
                printf("FAIL: thread %d write failed at key%d\n", thread_id, i);
                g_failCount += (int)batch.size();
                batch.clear();
                close(fd);
                return;
            }

            // 批量读回复（按 \r\n 分割计数）
            char buf[65536];
            std::string recvBuf;
            int okCount = 0;
            while (okCount < (int)batch.size()) {
                int r = read(fd, buf, sizeof(buf));
                if (r <= 0) {
                    g_failCount += (int)batch.size() - okCount;
                    break;
                }
                recvBuf.append(buf, r);
                while (okCount < (int)batch.size()) {
                    size_t pos = recvBuf.find("\r\n");
                    if (pos == std::string::npos) break;
                    okCount++;
                    recvBuf.erase(0, pos + 2);
                }
            }

            batch.clear();
        }
    }

    close(fd);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    int totalInsert = TOTAL_KEYS - START_KEY + 1;  // 50000
    printf("Inserting last %d keys (key%d~key%d, fast: %d threads, batch %d)...\n",
           totalInsert, START_KEY, TOTAL_KEYS, NUM_THREADS, BATCH_SIZE);

    auto start = std::chrono::steady_clock::now();

    // 每个线程负责一段 key（从 START_KEY 到 TOTAL_KEYS）
    int keysPerThread = totalInsert / NUM_THREADS;
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        int startKey = START_KEY + t * keysPerThread;
        int endKey = (t == NUM_THREADS - 1) ? TOTAL_KEYS : START_KEY + (t + 1) * keysPerThread - 1;
        threads.emplace_back(worker, t, startKey, endKey);
    }
    for (auto& th : threads) {
        th.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    printf("Insert complete. Time: %ld ms, QPS: %.0f\n",
           (long)ms, (double)totalInsert / (ms / 1000.0));
    printf("Fail: %d\n", g_failCount.load());
    return g_failCount.load() > 0 ? 1 : 0;
}