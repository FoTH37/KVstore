// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define MAX_DATA_SIZE 256

// 分阶段编译开关（用于定位 write 路径上的 eBPF 开销来源）：
//   STAGE=1：只做过滤（tgid + 长度 + 首字节），不读数据、不输出事件
//   STAGE=2：过滤 + 读 256B（不输出）
//   STAGE=3：完整（过滤 + 读 + 输出 perf 事件）—— 默认
#ifndef STAGE
#define STAGE 3
#endif

// 环形缓冲（ringbuf）：把捕获的 write 数据发给用户态
// 相比 perf event array：
//   1. 全局共享一份 ring（不再 per-CPU 各 64MB）
//   2. 支持 BPF_RB_NO_WAKEUP：提交时不唤醒用户态，省掉"每事件一次唤醒"的开销
//      （实测 perf 路径下该开销占 eBPF 总开销约 80%，见 STAGE 对照实验）
//   3. 用户态用 ring_buffer__poll 超时轮询消费（延迟 ≈ 轮询间隔）
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 26);   // 64MB
} write_events SEC(".maps");

// 需要观察的目标进程 tgid（阶段7-7 独立进程模式）：
// ebpf_proxy 独立进程加载本程序，把主库 kvserver 的 tgid 写入此 map，
// 只捕获该进程的 write()。ebpf_proxy 自己的转发 write 因 tgid 不同天然不被
// 捕获，反馈环自动消除（替代原 filter_fd 方案）。
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} filter_tgid SEC(".maps");

// 调试统计计数器（定位事件在哪一步被丢弃）：
//   0=进入程序  1=通过tgid过滤  2=通过长度过滤  3=首字节读取失败
//   4=首字节是'*'  5=数据读取失败  6=数据读取成功  7=perf输出失败
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

static __always_inline void stat_inc(int idx) {
    __u32 k = (__u32)idx;
    __u64 *c = bpf_map_lookup_elem(&stats, &k);
    if (c) __sync_fetch_and_add(c, 1);
}

SEC("tracepoint/syscalls/sys_enter_write")
int trace_write(struct trace_event_raw_sys_enter *ctx)
{
    stat_inc(0);
    __u32 count = (__u32)ctx->args[2];
    void *buf = (void *)ctx->args[1];
    int fd = (int)ctx->args[0];

    // 只观察目标进程（kvserver）的 write
    __u32 tgid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    __u32 zero = 0;
    __u32 *target = bpf_map_lookup_elem(&filter_tgid, &zero);
    if (!target || *target == 0 || tgid != *target)
        return 0;
    stat_inc(1);

    // 过滤：只处理长度 > 0 且 <= 256 的写入
    if (count == 0 || count > MAX_DATA_SIZE)
        return 0;
    stat_inc(2);

    // 先读第一个字节判断是否是 RESP 命令
    char first_byte = 0;
    if (bpf_probe_read_user(&first_byte, 1, buf) != 0) {
        stat_inc(3);
        return 0;
    }
    if (first_byte != '*')
        return 0;
    stat_inc(4);

#if STAGE >= 2
    // ★ kernel 5.4 verifier 硬限制：bpf_probe_read_user 的 size 必须是常量，
    //   变量 size 一律报 "invalid indirect read from stack"。
    //   所以固定读 256B（之前能跑通的形态），多余的 padding 由用户态 handle_event
    //   只解析第一条命令来丢弃，不会产生伪命令。
    char data[MAX_DATA_SIZE] = {0};   // 清零：读取失败时不把栈残留发给用户态
    if (bpf_probe_read_user(data, MAX_DATA_SIZE, buf) != 0) {
        stat_inc(5);
    }
    stat_inc(6);

#if STAGE >= 3
    // 提交到 ringbuf：BPF_RB_NO_WAKEUP = 不唤醒用户态（用户态定时轮询消费）
    // 变长提交：只交实际字节数（命令 ~40B，而非固定 256B），省 6 倍 ring 空间与拷贝
    __u32 out_len = count < MAX_DATA_SIZE ? count : MAX_DATA_SIZE;
    long oret = bpf_ringbuf_output(&write_events, data, out_len, BPF_RB_NO_WAKEUP);
    if (oret != 0)
        stat_inc(7);
#endif
#else
    (void)buf;
#endif

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
