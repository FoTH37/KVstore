#ifndef EBPF_SYNC_H
#define EBPF_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

// 启动 eBPF 监控器，监控指定 AOF 文件，将捕获的 write 数据通过 TCP 发送给从库
// aof_path: AOF 文件路径（如 "server_0_h.aof"）
// slave_ip: 从库 IP 地址
// slave_port: 从库端口
void startEbpfSync(const char* aof_path, const char* slave_ip, int slave_port);

// 通知 eBPF：全量同步已完成（收到 sync_finished），从缓存模式切换为转发模式
void ebpfNotifySyncFinished();

// 停止 eBPF 监控器
void stopEbpfSync();

#ifdef __cplusplus
}
#endif

#endif