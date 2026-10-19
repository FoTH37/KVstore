#include "nty_coroutine.h"
#include <arpa/inet.h>
#include <stdio.h>

typedef int (*msg_handler)(char *msg, int length, char *response);
static msg_handler kvs_handler;

void monitor(void *arg) {
    while (1) {
        sleep(1000);
    }
}

void server_reader(void *arg) {
    // 脱离调度器管理，使用阻塞 IO
    nty_coroutine_detach();

    int fd = (int)(intptr_t)arg;
    int ret = 0;

    while (1) {
        char buf[1024] = {0};
        ret = recv(fd, buf, 1024, 0);
        if (ret > 0) {
            char response[1024] = {0};
            int slength = kvs_handler(buf, ret, response);

            ret = send(fd, response, slength, 0);
            if (ret == -1) {
                close(fd);
                break;
            }
        } else if (ret == 0) {
            close(fd);
            break;
        }
    }
}

void server(void *arg) {
    // 脱离调度器管理，使用阻塞 IO
    nty_coroutine_detach();

    unsigned short port = *(unsigned short *)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local, remote;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

    listen(fd, 20);
    printf("listen port : %d\n", port);

    // 创建监控协程，确保调度器不退出
    nty_coroutine *monitor_co;
    nty_coroutine_create(&monitor_co, monitor, NULL);

    while (1) {
        socklen_t len = sizeof(struct sockaddr_in);
        int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
        if (cli_fd < 0) continue;

        nty_coroutine *read_co;
        nty_coroutine_create(&read_co, server_reader, (void*)(intptr_t)cli_fd);
    }
}

int ntyco_start(unsigned short port, msg_handler handler) {
    kvs_handler = handler;

    nty_coroutine *co = NULL;
    nty_coroutine_create(&co, server, &port);

    nty_schedule_run();
}