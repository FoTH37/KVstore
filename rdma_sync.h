#ifndef RDMA_SYNC_H
#define RDMA_SYNC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDMA_SYNC_PORT 2003
// RDMA 单次文件传输大小上限（256MB：64MB 在 250 万 key（RDB ~67MB）时已超限，
// 导致全量同步 "file size invalid" 失败；256MB 覆盖千万级 key）
#define RDMA_MAX_FILE_SIZE (256 * 1024 * 1024)

// ==================== 阶段7：文件传输接口 ====================

// 【从库端 · RDMA server】
// 启动 RDMA 接收服务：bind + listen 指定端口（可被多次 recv_file 复用）
// 返回 0 成功，-1 失败
int rdma_server_start(int port);

// 【从库端 · RDMA server】
// 接收一个文件并保存到 out_path（阻塞，等待一次 RDMA 连接）
// 返回接收到的文件字节数，-1 失败
int rdma_server_recv_file(int port, const char *out_path);

// 【主库端 · RDMA client】
// 发送本地文件 file_path 到远端 ip:port（阻塞，等待传输完成）
// 返回 0 成功，-1 失败
int rdma_client_send_file(const char *ip, int port, const char *file_path);

#ifdef __cplusplus
}
#endif

#endif
