#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

const int PORT = 2000;
const int TOTAL_KEYS = 100000;      // 总 key 数
const int NUM_THREADS = 16;         // 并发连接数（压测线程数）
const int BATCH_SIZE = 1000;        // 每个连接的批量大小

std::atomic<int> g_successCount(0);
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

// 每个线程负责一部分 key
void worker(int thread_id, int keys_per_thread) {
    int fd = connectServer();
    if (fd < 0) {
        printf("FAIL: thread %d connect failed\n", thread_id);
        return;
    }

    std::vector<std::string> batch;
    batch.reserve(BATCH_SIZE);

    int start = thread_id * keys_per_thread + 1;
    int end = start + keys_per_thread - 1;

    for (int i = start; i <= end; i++) {
        std::string key = "key" + std::to_string(i);
        std::string val = "val" + std::to_string(i);
        batch.push_back(buildSetCmd(key, val));

        if ((int)batch.size() == BATCH_SIZE || i == end) {
            // 批量发送
            std::string data;
            for (const auto& cmd : batch) data += cmd;

            ssize_t n = write(fd, data.data(), data.size());
            if (n != (ssize_t)data.size()) {
                g_failCount.fetch_add(batch.size());
                batch.clear();
                close(fd);
                return;
            }

            // 批量读回复（按 \r\n 分割计数，O(n)）
            char buf[65536];
            std::string recvBuf;
            int okCount = 0;
            while (okCount < (int)batch.size()) {
                int r = read(fd, buf, sizeof(buf));
                if (r <= 0) {
                    g_failCount.fetch_add(batch.size() - okCount);
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

            g_successCount.fetch_add(okCount);
            batch.clear();
        }
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    int numThreads = NUM_THREADS;
    if (argc > 1) numThreads = atoi(argv[1]);
    if (numThreads < 1) numThreads = 1;

    int keysPerThread = TOTAL_KEYS / numThreads;

    printf("Benchmark: %d SET operations, %d threads, %d keys/thread\n",
           TOTAL_KEYS, numThreads, keysPerThread);

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back(worker, t, keysPerThread);
    }
    for (auto& th : threads) {
        th.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double qps = (double)TOTAL_KEYS / (ms / 1000.0);

    printf("Success: %d, Fail: %d\n", g_successCount.load(), g_failCount.load());
    printf("Time: %ld ms\n", (long)ms);
    printf("QPS: %.0f\n", qps);
    return 0;
}