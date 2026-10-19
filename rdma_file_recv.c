#include <stdio.h>
#include <stdlib.h>
#include "rdma_sync.h"

// 用法: ./rdma_file_recv <port> <out_file>
// 从库端：启动 RDMA server 接收一个文件（阶段7-1 独立测试）
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <out_file>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_file = argv[2];

    printf("RDMA File Receiver: listening on port %d, output %s\n", port, out_file);
    int len = rdma_server_recv_file(port, out_file);
    if (len < 0) {
        fprintf(stderr, "rdma_server_recv_file failed\n");
        return 1;
    }
    printf("RDMA File Receiver: received %d bytes -> %s\n", len, out_file);
    return 0;
}
