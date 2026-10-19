#ifndef NTYNET_H
#define NTYNET_H

#include "BusinessHandler.h"

// 全量同步完成通知（RESP 格式），master/slave 共享，保证命令名/长度一致
#define SYNC_FINISHED_NAME "SYNC_FINISHED"              // 13 字节
#define SYNC_FINISHED_CMD  "*1\r\n$13\r\nSYNC_FINISHED\r\n"

// 通用 server 入口（master/slave 复用）
// handler: 业务处理器（多 worker 共享同一实例）
// port: 监听端口
// worker_count: worker 线程数（每线程一个独立调度器，SO_REUSEPORT 共享端口）
// read_only: true 时拦截写命令（从库 2004 查询端口）
void startServer(BusinessHandler* handler, int port, int worker_count = 1, bool read_only = false);

// 兼容包装：master 完整读写模式（等价 startServer(..., false)）
void startNtycoServer(BusinessHandler* handler, int port, int worker_count = 1);

// 设置 RDMA 从库地址（从配置读取）
void setRdmaConfig(const char* slave_ip, int slave_port);

#endif