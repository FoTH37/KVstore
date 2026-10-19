#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>
#include <fstream>
#include <atomic>
#include <thread>
#include <algorithm>
#include <jemalloc/jemalloc.h>
#include "mempool.h"

const int COUNT = 1000000;
const int OBJ_SIZE = 64;

struct TestObj {
    char data[OBJ_SIZE];
    TestObj() { memset(data, 0, OBJ_SIZE); }
};

// ===== 内存监控相关 =====
struct MemSample {
    long virt = 0;       // 当前 VmSize (KB)
    long rss = 0;        // 当前 VmRSS (KB)
    long virt_peak = 0;  // VmSize 峰值 (KB)
    long rss_peak = 0;   // VmRSS 峰值 (KB)
};

// 读取 /proc/self/status 的 VmSize 和 VmRSS（单位 KB）
static void readMemStats(long& virt, long& rss) {
    std::ifstream f("/proc/self/status");
    std::string line;
    virt = 0;
    rss = 0;
    while (std::getline(f, line)) {
        if (line.rfind("VmSize:", 0) == 0) {
            virt = std::stol(line.substr(7));
        } else if (line.rfind("VmRSS:", 0) == 0) {
            rss = std::stol(line.substr(6));
        }
    }
}

// 后台采样线程：每 10ms 采样一次，记录峰值
static void monitorThread(std::atomic<bool>& running, MemSample& sample) {
    long v, r;
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        readMemStats(v, r);
        sample.virt = v;
        sample.rss = r;
        if (v > sample.virt_peak) sample.virt_peak = v;
        if (r > sample.rss_peak) sample.rss_peak = r;
    }
}

// 打印内存状态
static void printMemStats(const char* name, long startV, long startR,
                          const MemSample& sample, long endV, long endR) {
    printf("%-15s VIRT: start=%6ldKB peak=%6ldKB end=%6ldKB | "
           "RSS: start=%6ldKB peak=%6ldKB end=%6ldKB\n",
           name, startV, sample.virt_peak, endV, startR, sample.rss_peak, endR);
}

void bench_glibc() {
    std::atomic<bool> running(true);
    MemSample sample;
    std::thread monitor(monitorThread, std::ref(running), std::ref(sample));

    long startVirt, startRss;
    readMemStats(startVirt, startRss);

    std::vector<TestObj*> ptrs;
    ptrs.reserve(COUNT);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < COUNT; i++) {
        TestObj* p = (TestObj*)malloc(sizeof(TestObj));
        if (p) {
            memset(p->data, 'a', OBJ_SIZE);
            ptrs.push_back(p);
        }
    }
    for (auto* p : ptrs) {
        free(p);
    }
    ptrs.clear();

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    running = false;
    monitor.join();

    long endVirt, endRss;
    readMemStats(endVirt, endRss);

    printf("glibc malloc:  %ld ms (%.2f ns/op)\n", ms, (double)ms * 1000000 / COUNT);
    printMemStats("glibc malloc", startVirt, startRss, sample, endVirt, endRss);
}

void bench_jemalloc() {
    std::atomic<bool> running(true);
    MemSample sample;
    std::thread monitor(monitorThread, std::ref(running), std::ref(sample));

    long startVirt, startRss;
    readMemStats(startVirt, startRss);

    std::vector<TestObj*> ptrs;
    ptrs.reserve(COUNT);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < COUNT; i++) {
        TestObj* p = (TestObj*)malloc(sizeof(TestObj));
        if (p) {
            memset(p->data, 'a', OBJ_SIZE);
            ptrs.push_back(p);
        }
    }
    for (auto* p : ptrs) {
        free(p);
    }
    ptrs.clear();

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    running = false;
    monitor.join();

    long endVirt, endRss;
    readMemStats(endVirt, endRss);

    printf("jemalloc:      %ld ms (%.2f ns/op)\n", ms, (double)ms * 1000000 / COUNT);
    printMemStats("jemalloc", startVirt, startRss, sample, endVirt, endRss);
}

void bench_pool() {
    ObjectPool<TestObj> pool;
    std::atomic<bool> running(true);
    MemSample sample;
    std::thread monitor(monitorThread, std::ref(running), std::ref(sample));

    long startVirt, startRss;
    readMemStats(startVirt, startRss);

    std::vector<TestObj*> ptrs;
    ptrs.reserve(COUNT);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < COUNT; i++) {
        TestObj* p = pool.allocate();
        if (p) {
            memset(p->data, 'a', OBJ_SIZE);
            ptrs.push_back(p);
        }
    }
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
    ptrs.clear();

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    running = false;
    monitor.join();

    long endVirt, endRss;
    readMemStats(endVirt, endRss);

    printf("ObjectPool:    %ld ms (%.2f ns/op)\n", ms, (double)ms * 1000000 / COUNT);
    printMemStats("ObjectPool", startVirt, startRss, sample, endVirt, endRss);
}

int main() {
    printf("Benchmark: %d alloc/dealloc cycles, %zu bytes each\n", COUNT, sizeof(TestObj));
    printf("------------------------------------------------\n");

#ifdef JEMALLOC_LINKED
    // 本程序链接了 -ljemalloc：进程内所有 malloc/free 被 jemalloc 符号替换，
    // 因此只有 jemalloc 行是真实有效的（"glibc malloc" 行实际也是 jemalloc 在跑，不打印）
    bench_jemalloc();
#else
    // 本程序使用系统 glibc：
    //  - "glibc malloc" 行真实有效
    //  - "ObjectPool" 行（底层同为 glibc malloc）用于与 jemalloc 版公平对比
    //  - "jemalloc" 行实际也是 glibc 在跑（未链接 jemalloc），不打印
    bench_glibc();
    bench_pool();
#endif

    printf("------------------------------------------------\n");
    return 0;
}