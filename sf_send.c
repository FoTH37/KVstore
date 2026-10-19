// sf_send.c —— sendfile 零拷贝发送测试（对比 RDMA/iperf3 用）
// 用法: ./sf_send <server_ip> <port> <file>
// 先起对端 ./sf_recv <port> <out_file>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <ip> <port> <file>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    const char *file = argv[3];

    int inFd = open(file, O_RDONLY);
    if (inFd < 0) { perror("open file"); return 1; }
    struct stat st;
    fstat(inFd, &st);
    off_t len = st.st_size;
    printf("SF send: %s (%lld bytes) -> %s:%d\n", file, (long long)len, ip, port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }

    // 发文件大小（8 字节大端）
    uint64_t net_len = htobe64((uint64_t)len);
    if (write(fd, &net_len, 8) != 8) { perror("write size"); return 1; }

    long t0 = now_ms();
    off_t offset = 0;
    ssize_t sent = 0;
    while (offset < len) {
        ssize_t n = sendfile(fd, inFd, &offset, (size_t)(len - offset));
        if (n < 0) { perror("sendfile"); return 1; }
        if (n == 0) break;
        sent += n;
    }
    // 等对端收完（对端回 1 字节确认）
    char ack;
    ssize_t r = read(fd, &ack, 1);
    long dt = now_ms() - t0;
    printf("SF send done: %lld bytes in %ld ms = %.1f MB/s\n",
           (long long)sent, dt, (double)sent / dt / 1000.0);
    (void)r;
    close(fd);
    close(inFd);
    return 0;
}
