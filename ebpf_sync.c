#define _GNU_SOURCE
#include "ebpf_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf_monitor.skel.h"

static pthread_t monitor_thread;
static volatile int g_stop = 0;
static volatile int g_sync_finished = 0;
static volatile int slave_fd = -1;
static int filter_fd_map_fd = -1;   // eBPF filter_fd map 的用户态 fd，connect 成功后写入 2000 转发 fd

// ===== 过滤 libbpf 噪音日志 =====
// bpf_create_map_xattr 首次加载（带 BTF）失败时 libbpf 会自动重试并打印
// "Retrying without BTF" 警告（errno=-524 非致命，重试后正常），属于每次启动必现的噪音。
// 过滤这两条，并只保留 WARN 级别（注意：本机 libbpf 0.5 的枚举顺序是
// LIBBPF_WARN=0 / INFO=1 / DEBUG=2，与新版相反，WARN 承载警告与错误；
// 设置自定义回调后 libbpf 会把 INFO/DEBUG 也交给回调，需要按级别截断），
// 其余 WARN 日志（如非 root 时的 "Operation not permitted"）保留。
static int libbpf_print_filter(enum libbpf_print_level level,
                               const char *format, va_list args) {
    if (level != LIBBPF_WARN) return 0;    // 只保留 WARN（警告/错误）
    if (format &&
        (strstr(format, "Retrying without BTF") || strstr(format, "bpf_create_map_xattr")))
        return 0;
    return vfprintf(stderr, format, args);
}



// ===== 提升 memlock 限制，确保 eBPF 在内核中能锁定内存 =====
static void raise_memlock_limit() {
    struct rlimit rlim;
    rlim.rlim_cur = RLIM_INFINITY;
    rlim.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rlim) != 0) {
        fprintf(stderr, "[ebpf_sync] Failed to raise memlock limit: %s\n", strerror(errno));
    }
}

static int connect_to_slave(const char *ip, int port) {
    while (!g_stop) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sleep(1); continue; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            printf("[ebpf_sync] Connected to slave %s:%d\n", ip, port);
            // 反馈环修复：把 2000 转发 fd 写入 eBPF filter_fd map，
            // 让 eBPF 跳过对这个 fd 的 write 捕获（否则转发数据→捕获→重新入队→再转发→自旋）。
            if (filter_fd_map_fd >= 0) {
                __u32 key = 0;
                __u32 val = (__u32)fd;
                int ret = bpf_map_update_elem(filter_fd_map_fd, &key, &val, BPF_ANY);
                if (ret != 0) {
                    fprintf(stderr, "[ebpf_sync] WARN: filter_fd map update failed: %s\n", strerror(errno));
                }
            }
            return fd;
        }
        close(fd);
        sleep(1);
    }
    return -1;
}

// 简单队列（显式长度字段，支持含 \r\n 的二进制 RESP 数据，不用 strlen）
#define QUEUE_SIZE 65536
#define QUEUE_ITEM_SIZE 256
static struct {
    char data[QUEUE_SIZE][QUEUE_ITEM_SIZE];
    unsigned int len[QUEUE_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} data_queue;

static void queue_init() {
    pthread_mutex_init(&data_queue.lock, NULL);
    pthread_cond_init(&data_queue.cond, NULL);
    data_queue.head = 0;
    data_queue.tail = 0;
}

static int queue_push(const char *data, int len) {
    if (len <= 0 || len > QUEUE_ITEM_SIZE) return -1;
    pthread_mutex_lock(&data_queue.lock);
    int next = (data_queue.tail + 1) % QUEUE_SIZE;
    if (next == data_queue.head) {
        pthread_mutex_unlock(&data_queue.lock);
        return -1;
    }
    memcpy(data_queue.data[data_queue.tail], data, len);
    data_queue.len[data_queue.tail] = (unsigned int)len;
    data_queue.tail = next;
    pthread_cond_signal(&data_queue.cond);
    pthread_mutex_unlock(&data_queue.lock);
    return 0;
}

// 非阻塞出队：队列空立即返回 -1（不等待），让 send_thread 能及时 flush 尾部 batch
static int queue_try_pop(char *buf, int buf_size) {
    pthread_mutex_lock(&data_queue.lock);
    if (data_queue.head == data_queue.tail) {
        pthread_mutex_unlock(&data_queue.lock);
        return -1;
    }
    int len = (int)data_queue.len[data_queue.head];
    if (len > buf_size) len = buf_size;
    memcpy(buf, data_queue.data[data_queue.head], len);
    data_queue.head = (data_queue.head + 1) % QUEUE_SIZE;
    pthread_mutex_unlock(&data_queue.lock);
    return len;
}

// 把 [data, data+len) 全部写入 fd（处理部分写入，保证不丢）
// 返回实际写入字节数；失败返回已写入字节数（< len）
static size_t write_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        // send + MSG_NOSIGNAL：写已断开连接时不触发 SIGPIPE
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[ebpf_sync] write_all failed at %zu/%zu: %s\n",
                    sent, len, strerror(errno));
            return sent;
        }
        sent += n;
    }
    return sent;
}

static void *send_thread_func(void *arg) {
    (void)arg;
    char buf[256];
    while (!g_stop && !g_sync_finished) {
        usleep(10000);
    }
    if (g_stop) return NULL;

    char batch[8192];   // 8KB 批量缓冲
    int batch_len = 0;
    // 修复：写失败时保存未发送部分，重连成功后先重发（零丢失双保险）
    static char pending_buf[8192];
    static int pending_len = 0;

    // 长驻转发线程：永不因"暂时无数据"退出，只随 g_stop 退出
    while (!g_stop) {
        // 连接未就绪/已断开：保留 batch，等 connect_thread 重连
        if (slave_fd < 0) {
            usleep(10000);
            continue;
        }

        // 修复：优先重发上次写失败未完成的部分（保证数据不丢）
        if (pending_len > 0) {
            size_t w = write_all(slave_fd, pending_buf, pending_len);
            if (w != (size_t)pending_len) {
                memmove(pending_buf, pending_buf + w, pending_len - w);
                pending_len -= (int)w;
                fprintf(stderr, "[ebpf_sync] WARN: pending write failed %zu/%d, closing conn for reconnect\n",
                        w, pending_len + (int)w);
                close(slave_fd);
                slave_fd = -1;
                continue;
            }
            pending_len = 0;
        }

        int len = queue_try_pop(buf, sizeof(buf));
        if (len <= 0) {
            // 队列空：把剩余批量 flush 出去
            if (batch_len > 0) {
                size_t w = write_all(slave_fd, batch, batch_len);
                if (w != (size_t)batch_len) {
                    // 写失败：保存未发送部分，关闭并置 -1，让 connect_thread 重连
                    memcpy(pending_buf, batch + w, batch_len - w);
                    pending_len = batch_len - (int)w;
                    fprintf(stderr, "[ebpf_sync] WARN: batch write failed %zu/%d, saved %d bytes pending, closing conn for reconnect\n",
                            w, batch_len, pending_len);
                    close(slave_fd);
                    slave_fd = -1;
                }
                batch_len = 0;
            }
            usleep(1000);
            continue;
        }

        // 拼到批量缓冲
        if (batch_len + len <= (int)sizeof(batch)) {
            memcpy(batch + batch_len, buf, len);
            batch_len += len;
        } else {
            // 批量缓冲满：先发出去再拼
            size_t w = write_all(slave_fd, batch, batch_len);
            if (w != (size_t)batch_len) {
                memcpy(pending_buf, batch + w, batch_len - w);
                pending_len = batch_len - (int)w;
                fprintf(stderr, "[ebpf_sync] WARN: batch write failed %zu/%d, saved %d bytes pending, closing conn for reconnect\n",
                        w, batch_len, pending_len);
                close(slave_fd);
                slave_fd = -1;
            }
            batch_len = 0;
            memcpy(batch, buf, len);
            batch_len = len;
        }
    }
    return NULL;
}

// 参数先复制到栈上，避免 monitor_thread 退出 free(params) 后悬垂
static void *connect_thread_func(void *arg) {
    char **params = (char **)arg;
    char ip[64];
    int port;
    snprintf(ip, sizeof(ip), "%s", params[1]);
    port = atoi(params[2]);

    // 常驻守护：连上后监控 slave_fd，发现被 send_thread 置 -1（连接失效）就重连
    while (!g_stop) {
        int fd = connect_to_slave(ip, port);   // 阻塞直到连上或 g_stop
        if (g_stop) break;
        if (fd < 0) break;
        slave_fd = fd;
        printf("[ebpf_sync] slave connection established (fd=%d)\n", fd);
        // 等待连接失效：send_thread 写失败时会 close(slave_fd) 并置 -1
        // 修复：定期用 recv(MSG_PEEK) 探测连接活性（对端 close 会读到 EOF/错误），
        //       避免"连接已死但 slave_fd 仍 >= 0"导致增量通道静默失效、
        //       以及重连窗口第一批数据丢失。
        while (!g_stop && slave_fd >= 0) {
            usleep(500000);   // 500ms 探测一次
            if (g_stop || slave_fd < 0) break;
            char probe;
            ssize_t r = recv(slave_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) {
                // 对端已关闭（FIN）：连接死亡，立即重连
                fprintf(stderr, "[ebpf_sync] connection EOF detected (fd=%d), reconnecting...\n", slave_fd);
                close(slave_fd);
                slave_fd = -1;
                break;
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                // 连接错误（ECONNRESET/EPIPE/ENOTCONN 等）：连接死亡，立即重连
                fprintf(stderr, "[ebpf_sync] connection error detected (fd=%d): %s, reconnecting...\n",
                        slave_fd, strerror(errno));
                close(slave_fd);
                slave_fd = -1;
                break;
            }
            // r > 0：有数据可读（理论上从库不回写 2000），连接正常；MSG_PEEK 不消费数据
        }
        if (g_stop) break;
        printf("[ebpf_sync] slave connection lost, reconnecting...\n");
    }
    return NULL;
}


// 解析一个完整 RESP 命令的字节数（含结尾 \r\n）。
// data 从 '*' 开始；返回命令总字节数，0 表示不完整/非法（event 内截断或垃圾）。
static int extract_resp_cmd(const char *data, int size) {
    if (size <= 0 || data[0] != '*') return 0;
    int pos = 1;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') pos++;
    if (pos >= size || data[pos] != '\r' || pos + 1 >= size || data[pos + 1] != '\n') return 0;
    int elem_count = 0;
    for (int i = 1; i < pos; i++) elem_count = elem_count * 10 + (data[i] - '0');
    if (elem_count <= 0 || elem_count > 1024) return 0;
    pos += 2;  // 跳过 \r\n
    for (int i = 0; i < elem_count; i++) {
        if (pos >= size || data[pos] != '$') return 0;
        pos++;
        int str_len = 0;
        int has_len = 0;
        while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
            str_len = str_len * 10 + (data[pos] - '0');
            has_len = 1;
            pos++;
        }
        if (!has_len || pos >= size || data[pos] != '\r' || pos + 1 >= size || data[pos + 1] != '\n') return 0;
        pos += 2;  // 跳过 \r\n
        if (str_len < 0 || pos + str_len + 2 > size) return 0;
        pos += str_len;
        if (data[pos] != '\r' || pos + 1 >= size || data[pos + 1] != '\n') return 0;
        pos += 2;  // 跳过内容后的 \r\n
    }
    return pos;
}

static void handle_event(void *ctx, int cpu, void *data, __u32 size) {
    (void)ctx; (void)cpu;
    if (g_stop) return;

    char *str = (char *)data;
    if (size <= 0 || str[0] != '*') return;

    // 每个 perf event = 主库 broadcast 的一次 write() = 一条完整命令。
    // 只解析第一条完整命令，event 尾部（固定 256B 读入带来的堆残留/padding）
    // 直接丢弃——避免把残留误解析成伪命令（伪命令会让 slave applied 虚增、
    // 真实 key 缺失，这是之前 152 缺失的最大嫌疑）。
    int cmd_len = extract_resp_cmd(str, (int)size);
    if (cmd_len <= 0) return;

    // 入队转发；队列满时静默丢弃（QUEUE_SIZE=65536，正常负载不会触发）
    (void)queue_push(str, cmd_len);
}

static void *monitor_thread_func(void *arg) {
    char **params = (char **)arg;
    const char *aof_path = params[0];

    queue_init();

    // ===== 加载 eBPF 前先提升 memlock 限制 =====
    raise_memlock_limit();

    // 过滤 libbpf 的 BTF 重试噪音日志（见 libbpf_print_filter）
    libbpf_set_print(libbpf_print_filter);

    // 阶段7-5：先加载 eBPF（立即开始捕获到队列），不再被 connect_to_slave 阻塞
    struct ebpf_monitor_bpf *skel = ebpf_monitor_bpf__open();
    if (!skel) goto cleanup_params;

    int err = ebpf_monitor_bpf__load(skel);
    if (err) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    err = ebpf_monitor_bpf__attach(skel);
    if (err) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    // 反馈环修复：保存 filter_fd map 的 fd，connect_to_slave 连接成功后写入 2000 转发 fd
    filter_fd_map_fd = bpf_map__fd(skel->maps.filter_fd);

    struct perf_buffer *pb = NULL;
#if defined(LIBBPF_MAJOR_VERSION) && LIBBPF_MAJOR_VERSION >= 1
    // 新版 libbpf (>=1.0)：加大 perf ring 到 64MB/CPU，抗 8 线程并发广播突发
    pb = perf_buffer__new(bpf_map__fd(skel->maps.write_events), 16384,
                          handle_event, NULL, NULL, NULL);
#else
    // 旧版 libbpf (<1.0)
    struct perf_buffer_opts opts = {
        .sample_cb = (void *)handle_event,
    };
    pb = perf_buffer__new(bpf_map__fd(skel->maps.write_events), 16384, &opts);
#endif
    if (!pb) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    // 阶段7-5：后台线程负责连 slave（无限重试），不阻塞 eBPF 捕获
    pthread_t conn_thread;
    pthread_create(&conn_thread, NULL, connect_thread_func, params);
    pthread_detach(conn_thread);

    // 发送线程：等 sync_finished 后转发（缓存模式见 send_thread_func）
    pthread_t send_thread;
    pthread_create(&send_thread, NULL, send_thread_func, NULL);
    pthread_detach(send_thread);

    while (!g_stop) {
        err = perf_buffer__poll(pb, 200);
        if (err < 0 && err != -EINTR) break;
    }

    perf_buffer__free(pb);
    ebpf_monitor_bpf__destroy(skel);
    filter_fd_map_fd = -1;
    if (slave_fd >= 0) close(slave_fd);
    slave_fd = -1;

cleanup_params:
    (void)aof_path;
    free(params[0]);
    free(params[1]);
    free(params[2]);
    free(params);
    return NULL;
}

void startEbpfSync(const char* aof_path, const char* slave_ip, int slave_port) {
    if (g_stop) return;
    g_stop = 0;

    char **params = (char **)malloc(3 * sizeof(char *));
    params[0] = strdup(aof_path);
    params[1] = strdup(slave_ip);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", slave_port);
    params[2] = strdup(port_str);

    pthread_create(&monitor_thread, NULL, monitor_thread_func, params);
    pthread_detach(monitor_thread);
}

void stopEbpfSync() {
    g_stop = 1;
}

void ebpfNotifySyncFinished() {
    g_sync_finished = 1;
    printf("[ebpf_sync] full sync finished, switching to forward mode\n");
}
