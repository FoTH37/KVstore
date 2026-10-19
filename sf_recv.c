// sf_recv.c —— sendfile 测试接收端
// 用法: ./sf_recv <port> <out_file>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <out_file>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *outFile = argv[2];

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 1) < 0) { perror("listen"); return 1; }
    printf("SF recv: listening %d -> %s\n", port, outFile);

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { perror("accept"); return 1; }

    // 读文件大小
    uint64_t net_len;
    ssize_t got = 0;
    while (got < 8) {
        ssize_t r = read(cfd, (char*)&net_len + got, 8 - got);
        if (r <= 0) { perror("read size"); return 1; }
        got += r;
    }
    uint64_t len = be64toh(net_len);
    printf("SF recv: expecting %llu bytes\n", (unsigned long long)len);

    int ofd = open(outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 1; }

    long t0 = now_ms();
    char buf[65536];
    uint64_t total = 0;
    while (total < len) {
        ssize_t r = read(cfd, buf, sizeof(buf));
        if (r <= 0) break;
        ssize_t w = write(ofd, buf, (size_t)r);
        if (w != r) break;
        total += (uint64_t)r;
    }
    close(ofd);
    // 回确认
    write(cfd, "1", 1);
    long dt = now_ms() - t0;
    printf("SF recv done: %llu bytes in %ld ms = %.1f MB/s\n",
           (unsigned long long)total, dt, (double)total / dt / 1000.0);
    close(cfd);
    close(lfd);
    return 0;
}
