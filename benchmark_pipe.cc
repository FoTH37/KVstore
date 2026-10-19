#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <vector>
#include <chrono>

const int PORT = 2000;
const int TOTAL_KEYS = 100000;
const int BATCH_SIZE = 1000;

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

// 批量发送命令
bool sendBatch(int fd, const std::vector<std::string>& cmds) {
    std::string data;
    for (const auto& cmd : cmds) {
        data += cmd;
    }
    ssize_t total = 0;
    while (total < (ssize_t)data.size()) {
        ssize_t n = write(fd, data.data() + total, data.size() - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}


// 批量读取回复（读够 count 条回复为止）
bool recvReplies(int fd, int count) {
    char buf[65536];
    std::string recvBuf;
    int okCount = 0;
    while (okCount < count) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) return false;
        recvBuf.append(buf, n);
        // 逐条解析 RESP 回复
        while (okCount < count) {
            if (recvBuf.empty()) break;
            // 简单回复解析：+OK\r\n 是 5 字节
            if (recvBuf[0] == '+' && recvBuf.size() >= 5) {
                if (recvBuf.substr(0, 5) == "+OK\r\n") {
                    okCount++;
                    recvBuf.erase(0, 5);
                    continue;
                }
            }
            // 如果不是简单+OK，或者不完整，跳出等待更多数据
            break;
        }
    }
    return okCount == count;
}

int main() {
    int fd = connectServer();
    if (fd < 0) {
        printf("FAIL: connect\n");
        return 1;
    }

    printf("Benchmark: %d SET operations (pipeline, batch=%d)\n", TOTAL_KEYS, BATCH_SIZE);
    auto start = std::chrono::steady_clock::now();

    std::vector<std::string> batch;
    batch.reserve(BATCH_SIZE);
    for (int i = 0; i < TOTAL_KEYS; i++) {
        std::string key = "key" + std::to_string(i);
        std::string val = "val" + std::to_string(i);
        batch.push_back(buildSetCmd(key, val));

        if ((i + 1) % BATCH_SIZE == 0 || i == TOTAL_KEYS - 1) {
            int batchIndex = i / BATCH_SIZE;
            printf("Batch %d: sending %zu commands (key%d~key%d)\n", 
                   batchIndex, batch.size(), i - (int)batch.size() + 1, i);
            if (!sendBatch(fd, batch)) {
                printf("FAIL: send at key%d\n", i);
                close(fd);
                return 1;
            }
            if (!recvReplies(fd, (int)batch.size())) {
                printf("FAIL: recv at key%d\n", i);
                close(fd);
                return 1;
            }
            batch.clear();
            if ((i + 1) % 20000 == 0) {
                printf("  processed %d keys\n", i + 1);
            }
        }
    }

    close(fd);
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double qps = (double)TOTAL_KEYS / (ms / 1000.0);

    printf("Time: %ld ms\n", (long)ms);
    printf("QPS: %.0f\n", qps);
    return 0;
}