#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf_monitor.skel.h"

// perf buffer 回调：收到 eBPF 程序发来的数据时被调用
static void handle_event(void *ctx, int cpu, void *data, __u32 size)
{
    // data 就是 eBPF 程序捕获的 write 数据
    printf("[eBPF] Captured %u bytes: ", size);
    // 尝试以字符串形式打印（RESP 格式的数据大部分是 ASCII）
    char *str = (char *)data;
    for (__u32 i = 0; i < size && i < 100; i++) {
        if (str[i] >= 32 && str[i] < 127)
            putchar(str[i]);
        else
            printf("\\x%02x", (unsigned char)str[i]);
    }
    printf("\n");
}

// perf buffer 丢失回调：当数据丢失时被调用
static void handle_lost(void *ctx, int cpu, __u64 lost_cnt)
{
    printf("[eBPF] Lost %llu events on CPU %d\n", (unsigned long long)lost_cnt, cpu);
}

int main(int argc, char *argv[])
{
    struct ebpf_monitor_bpf *skel = NULL;
    struct perf_buffer *pb = NULL;
    struct perf_buffer_opts opts = {0};
    int err;

    // 1. 打开并加载 eBPF 程序
    skel = ebpf_monitor_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    err = ebpf_monitor_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF skeleton: %d\n", err);
        goto cleanup;
    }

    // 2. 挂载 eBPF 程序到跟踪点
    err = ebpf_monitor_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF skeleton: %d\n", err);
        goto cleanup;
    }

    printf("eBPF monitor started. Capturing all write() calls.\n");
    printf("Press Ctrl+C to stop.\n");

    // 3. 设置 perf buffer 来接收事件（旧版 API）
    opts.sample_cb = (void *)handle_event;
    opts.lost_cb = (void *)handle_lost;
    pb = perf_buffer__new(bpf_map__fd(skel->maps.write_events),
                          8, &opts);
    if (!pb) {
        fprintf(stderr, "Failed to create perf buffer\n");
        goto cleanup;
    }

    // 4. 轮询接收事件
    while (1) {
        err = perf_buffer__poll(pb, 100);  // 每 100ms 检查一次
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "perf_buffer__poll failed: %s\n", strerror(-err));
            break;
        }
    }

cleanup:
    if (pb)
        perf_buffer__free(pb);
    if (skel)
        ebpf_monitor_bpf__destroy(skel);
    return err ? 1 : 0;
}