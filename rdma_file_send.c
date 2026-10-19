#include <stdio.h>
#include <stdlib.h>
#include "rdma_sync.h"

// 用法: ./rdma_file_send <ip> <port> <file>
// 主库端：通过 RDMA 发送一个文件（阶段7-1 独立测试）
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <ip> <port> <file>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    const char *file = argv[3];

    printf("RDMA File Sender: sending %s to %s:%d\n", file, ip, port);
    if (rdma_client_send_file(ip, port, file) != 0) {
        fprintf(stderr, "rdma_client_send_file failed\n");
        return 1;
    }
    printf("RDMA File Sender: send completed\n");
    return 0;
}
