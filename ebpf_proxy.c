// ebpf_proxy.c —— eBPF 增量转发独立进程（多通道版）
//
// 职责：加载 eBPF（需要 root），按 tgid 只观察主库 kvserver 进程的 write()
//       系统调用，捕获其广播给从库的 RESP 写命令（SET/DEL 等），
//       经 perf buffer → 队列 → TCP 转发到从库统一端口（默认 2000）。
//
// 用法：sudo ./ebpf_proxy <kvserver_pid> <从库IP> [端口=2000] [通道数]
//   例：sudo ./ebpf_proxy 12345 192.168.1.44 2000
//   通道数优先取命令行第 4 参，否则读 kvstore.conf 的 ebpf_channels（默认 1）
//
// ===== 多通道设计 =====
//   单通道时全链路的硬瓶颈在从库：一条 TCP 连接只能由一个协程顺序解析执行
//   （约 5 万 cmd/s），而主库广播可达 6~7 万/s → 必然积压。
//
//   多通道把"解析 → 队列 → TCP"复制 N 份：
//     monitor 线程解析出命令的 key → hash(key) % N 选通道 → 该通道队列
//     每通道一个 send 线程，只写自己的 TCP 连接
//     从库侧每条连接一个协程 → N 路并行解析执行
//
//   保序：同 key 恒定哈希到同一通道 → 同一 send 线程顺序写出 → 从库同一协程
//         顺序执行；跨 key 的顺序不再保证（增量同步不需要）。
//   前提：N 运行中不变（变更会使同 key 换通道 → 乱序）。
//
// 设计要点：
//   1. 独立进程：kvserver 不再内嵌 eBPF（不再需要 root）
//   2. filter_tgid：BPF 只观察 kvserver 进程的 write，本进程自己的转发
//      write 因 tgid 不同天然不被捕获 → 反馈环自动消除（替代原 filter_fd）
//   3. 队列节点来自分块池（chunk 池）：malloc 次数从"每条命令一次"降到
//      "每 4096 条一次"；池满自动开新 chunk，不丢数据
//   4. 连接活性检测：connect_thread（单线程管 N 条）每 500ms recv(MSG_PEEK)
//      探测，断开自动重连；send_thread 写失败保存 pending，重连后先重发
//   5. drain_thread：读走从库回执（不读会写满从库发送缓冲 → 从库整条连接停摆）

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf_monitor.skel.h"

#define MAX_CHANNELS    8      // 通道数上限
#define CHUNK_NODES     4096   // 每个 chunk 的节点数
#define QUEUE_ITEM_SIZE 256    // 单条命令最大长度（与 BPF 侧 MAX_DATA_SIZE 一致）

static volatile int g_stop = 0;
static volatile int g_slave_fd[MAX_CHANNELS];   // 每通道一条到从库的 TCP（-1 = 未连接）
static int g_channels = 1;                      // 通道数（运行中不变）
static int filter_tgid_map_fd = -1;             // filter_tgid map 的用户态 fd

// ===== 每通道的 pending（写失败未发完的数据，重连后先补发）=====
// 每个下标只被对应通道的 send 线程访问，无需额外加锁
static char g_pending_buf[MAX_CHANNELS][8192];
static int  g_pending_len[MAX_CHANNELS];

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
        fprintf(stderr, "[ebpf_proxy] Failed to raise memlock limit: %s\n", strerror(errno));
    }
}

// ===== 极简配置读取：从 kvstore.conf 取 key=value 的整数值 =====
static int conf_get_int(const char *path, const char *key, int def) {
    FILE *f = fopen(path, "r");
    if (!f) return def;
    char line[512];
    size_t klen = strlen(key);
    int val = def;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            val = atoi(p + klen + 1);
            break;
        }
    }
    fclose(f);
    return val;
}

// ===== FNV-1a 哈希：用于把 key 映射到通道（同 key 恒定同通道）=====
static unsigned int fnv1a(const char *s, int len) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

// =====================================================================
//  分块池（chunk 池）+ 每通道无界队列
//  旧实现每条命令 malloc 一个节点，深积压时 malloc 成为可观开销；
//  改为按 chunk 批量分配：一 chunk = 4096 个节点，用尽自动开新 chunk。
//  池满不丢数据（继续开 chunk），节点归还到它所属 chunk 的空闲链表。
// =====================================================================
typedef struct QueueNode {
    char data[QUEUE_ITEM_SIZE];
    unsigned int len;
    struct QueueNode *next;
    struct Chunk *chunk;          // 所属 chunk（free 时归还到这里）
} QueueNode;

typedef struct Chunk {
    struct Chunk *next;
    QueueNode *free_list;         // 本 chunk 的空闲节点链表
    QueueNode nodes[CHUNK_NODES];
} Chunk;

typedef struct {
    QueueNode *head;
    QueueNode *tail;
    Chunk *chunks;                // chunk 链表（保留复用，不回收）
    Chunk *cur;                   // 当前分配用的 chunk
    unsigned long long push_cnt;
    unsigned long long pop_cnt;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} ChannelQueue;

static ChannelQueue g_q[MAX_CHANNELS];

// 新建 chunk 并挂到链表头（调用者需持有该通道的锁）
static Chunk *chunk_new(Chunk **list) {
    Chunk *c = (Chunk *)malloc(sizeof(Chunk));
    if (!c) return NULL;
    c->next = *list;
    c->free_list = NULL;
    for (int i = CHUNK_NODES - 1; i >= 0; i--) {
        c->nodes[i].chunk = c;
        c->nodes[i].next = c->free_list;
        c->free_list = &c->nodes[i];
    }
    *list = c;
    return c;
}

// 取一个空闲节点（调用者需持有该通道的锁）
static QueueNode *node_alloc(ChannelQueue *q) {
    if (!q->cur || !q->cur->free_list) {
        if (!chunk_new(&q->chunks)) return NULL;   // 仅内存耗尽才失败
        q->cur = q->chunks;
    }
    QueueNode *n = q->cur->free_list;
    q->cur->free_list = n->next;
    n->next = NULL;
    return n;
}

// 归还节点到所属 chunk（调用者需持有该通道的锁）
static void node_free(QueueNode *n) {
    Chunk *c = n->chunk;
    n->next = c->free_list;
    c->free_list = n;
}

static void channel_init(int ch) {
    ChannelQueue *q = &g_q[ch];
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static int channel_push(int ch, const char *data, int len) {
    if (len <= 0 || len > QUEUE_ITEM_SIZE) return -1;
    ChannelQueue *q = &g_q[ch];
    pthread_mutex_lock(&q->lock);
    QueueNode *node = node_alloc(q);
    if (!node) {                       // 只剩"内存耗尽"才失败
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    memcpy(node->data, data, len);
    node->len = (unsigned int)len;
    node->next = NULL;
    if (q->tail) q->tail->next = node;
    else q->head = node;
    q->tail = node;
    q->push_cnt++;
    pthread_cond_signal(&q->cond);     // 唤醒该通道的 send 线程
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static int channel_pop(int ch, char *buf, int buf_size) {
    ChannelQueue *q = &g_q[ch];
    pthread_mutex_lock(&q->lock);
    QueueNode *node = q->head;
    if (!node) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    int len = (int)node->len;
    if (len > buf_size) len = buf_size;
    memcpy(buf, node->data, len);
    q->head = node->next;
    if (!q->head) q->tail = NULL;      // 队列空时 tail 必须一起清
    q->pop_cnt++;
    node_free(node);
    pthread_mutex_unlock(&q->lock);
    return len;
}

static unsigned long long channel_depth(int ch) {
    ChannelQueue *q = &g_q[ch];
    pthread_mutex_lock(&q->lock);
    unsigned long long d = q->push_cnt - q->pop_cnt;
    pthread_mutex_unlock(&q->lock);
    return d;
}

// =====================================================================
//  从库连接：单线程管 N 条（建连 / 探活 / 重连）
// =====================================================================
// 单次非阻塞 connect（带 1s 超时），成功返回阻塞模式 fd，失败返回 -1。
// 非阻塞是必需的：多通道下若某条连接阻塞重试，会拖住其他通道的建连。
static int try_connect_once(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        struct pollfd p;
        p.fd = fd; p.events = POLLOUT; p.revents = 0;
        if (poll(&p, 1, 1000) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            return -1;
        }
    }

    // 恢复阻塞模式：send 线程依赖阻塞 fd 的"完整写出"语义
    fcntl(fd, F_SETFL, flags);
    return fd;
}

static void *connect_thread_func(void *arg) {
    char **params = (char **)arg;
    char ip[64];
    int port;
    snprintf(ip, sizeof(ip), "%s", params[1]);
    port = atoi(params[2]);

    while (!g_stop) {
        for (int i = 0; i < g_channels; i++) {
            if (g_slave_fd[i] < 0) {
                int fd = try_connect_once(ip, port);
                if (fd >= 0) {
                    g_slave_fd[i] = fd;
                    printf("[ebpf_proxy] ch%d connected to slave %s:%d (fd=%d)\n",
                           i, ip, port, fd);
                }
                continue;
            }
            // 探活：MSG_PEEK 只探测不消费（回执留给 drain 线程读）
            char probe;
            ssize_t r = recv(g_slave_fd[i], &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) {
                fprintf(stderr, "[ebpf_proxy] ch%d connection EOF (fd=%d), reconnecting...\n",
                        i, g_slave_fd[i]);
                close(g_slave_fd[i]);
                g_slave_fd[i] = -1;
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fprintf(stderr, "[ebpf_proxy] ch%d connection error (fd=%d): %s, reconnecting...\n",
                        i, g_slave_fd[i], strerror(errno));
                close(g_slave_fd[i]);
                g_slave_fd[i] = -1;
            }
        }
        usleep(500000);   // 每 500ms 轮询一轮（建连 / 探活）
    }
    return NULL;
}

// =====================================================================
//  回执排空线程：poll N 条连接，读走从库的 +OK 丢弃
//  从库 client_handler 对每条命令都回 +OK（5B）；本进程只写不读 →
//  从库发送缓冲很快写满 → 其 flushReply 阻塞 → 从库整条连接停摆。
// =====================================================================
static void *drain_thread_func(void *arg) {
    (void)arg;
    char buf[16384];
    struct pollfd pfds[MAX_CHANNELS];

    while (!g_stop) {
        int n = 0;
        for (int i = 0; i < g_channels; i++) {
            pfds[n].fd = g_slave_fd[i];
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
        int r = poll(pfds, (nfds_t)n, 200);
        if (r <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (pfds[i].fd < 0) continue;
            // POLLHUP/POLLERR 也算可读（recv 会返回 0 或错误），需读走避免空转
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t got = recv(pfds[i].fd, buf, sizeof(buf), 0);
            if (got > 0) continue;                     // 丢弃回执
            if (got == 0) continue;                    // 对端关闭：交给 connect 线程重连
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            usleep(50000);                             // EBADF 等：fd 正被更换
        }
    }
    return NULL;
}

// =====================================================================
//  发送线程（每通道一个）：只取本通道队列，只写本通道连接
// =====================================================================
static size_t write_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[ebpf_proxy] write_all failed at %zu/%zu: %s\n",
                    sent, len, strerror(errno));
            return sent;
        }
        sent += (size_t)n;
    }
    return sent;
}

static void *send_thread_func(void *arg) {
    int ch = (int)(long)arg;
    char buf[QUEUE_ITEM_SIZE];
    char batch[8192];                       // 8KB 批量缓冲
    int batch_len = 0;
    char *pending = g_pending_buf[ch];      // 本通道独享
    int *pending_len = &g_pending_len[ch];

    while (!g_stop) {
        int fd = g_slave_fd[ch];
        if (fd < 0) {
            usleep(10000);
            continue;
        }

        // 优先重发上次写失败未完成的部分（重连后从这里恢复）
        if (*pending_len > 0) {
            size_t w = write_all(fd, pending, *pending_len);
            if (w != (size_t)*pending_len) {
                memmove(pending, pending + w, *pending_len - w);
                *pending_len -= (int)w;
                fprintf(stderr, "[ebpf_proxy] ch%d pending write failed %zu/%d, closing conn\n",
                        ch, w, *pending_len + (int)w);
                close(fd);
                g_slave_fd[ch] = -1;
                continue;
            }
            *pending_len = 0;
        }

        int len = channel_pop(ch, buf, sizeof(buf));
        if (len <= 0) {
            if (batch_len > 0) {
                size_t w = write_all(fd, batch, batch_len);
                if (w != (size_t)batch_len) {
                    memcpy(pending, batch + w, batch_len - w);
                    *pending_len = batch_len - (int)w;
                    fprintf(stderr, "[ebpf_proxy] ch%d batch write failed %zu/%d, saved %d bytes\n",
                            ch, w, batch_len, *pending_len);
                    close(fd);
                    g_slave_fd[ch] = -1;
                }
                batch_len = 0;
            }
            usleep(1000);
            continue;
        }

        if (batch_len + len <= (int)sizeof(batch)) {
            memcpy(batch + batch_len, buf, len);
            batch_len += len;
        } else {
            size_t w = write_all(fd, batch, batch_len);
            if (w != (size_t)batch_len) {
                memcpy(pending, batch + w, batch_len - w);
                *pending_len = batch_len - (int)w;
                fprintf(stderr, "[ebpf_proxy] ch%d batch write failed %zu/%d, saved %d bytes\n",
                        ch, w, batch_len, *pending_len);
                close(fd);
                g_slave_fd[ch] = -1;
            }
            batch_len = 0;
            memcpy(batch, buf, len);
            batch_len = len;
        }
    }
    return NULL;
}

// =====================================================================
//  RESP 解析：取出第一条完整命令的长度，并取出第 2 个参数（key）
//  命令形如：*3\r\n$3\r\nSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<value>\r\n
//  第 0 个参数 = 命令名，第 1 个参数 = key（MSET 在主库侧已拆成逐条 SET）
// =====================================================================
static int parse_resp_first(const char *data, int size,
                            char *key_out, int key_max, int *key_len_out) {
    *key_len_out = 0;
    if (size <= 0 || data[0] != '*') return 0;

    int pos = 1;
    int elem_count = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        elem_count = elem_count * 10 + (data[pos] - '0');
        pos++;
    }
    if (pos >= size || data[pos] != '\r' || pos + 1 >= size || data[pos + 1] != '\n') return 0;
    if (elem_count <= 0 || elem_count > 1024) return 0;
    pos += 2;

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
        pos += 2;
        if (str_len < 0 || pos + str_len + 2 > size) return 0;

        if (i == 1) {                       // 第 2 个参数 = key
            int n = (str_len < key_max) ? str_len : key_max;
            memcpy(key_out, data + pos, n);
            *key_len_out = n;
        }

        pos += str_len;
        if (data[pos] != '\r' || pos + 1 >= size || data[pos + 1] != '\n') return 0;
        pos += 2;
    }
    return pos;                             // 第一条完整命令的总长度
}

static int handle_event(void *ctx, void *data, size_t size) {
    (void)ctx;
    if (g_stop) return 0;

    char *str = (char *)data;
    if (size == 0 || str[0] != '*') {
        // 计数：数据残缺或非 RESP 内容被丢弃
        static unsigned long long bad_head = 0;
        bad_head++;
        if (bad_head <= 10) {
            char dump[64];
            int n = (size < 32) ? (int)size : 32;
            int p = 0;
            for (int i = 0; i < n && p < (int)sizeof(dump) - 1; i++) {
                unsigned char c = (unsigned char)str[i];
                dump[p++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            dump[p] = '\0';
            fprintf(stderr, "[ebpf_proxy] BAD HEAD #%llu size=%zu head=[%s]\n",
                    bad_head, size, dump);
        }
        return 0;
    }

    // 每个 perf event = 主库 broadcast 的一次 write() = 一条完整命令。
    // 只解析第一条完整命令，event 尾部（固定 256B 读入带来的堆残留/padding）直接丢弃
    char key[QUEUE_ITEM_SIZE];
    int key_len = 0;
    int cmd_len = parse_resp_first(str, (int)size, key, (int)sizeof(key), &key_len);
    if (cmd_len <= 0) {
        // 调试：解析失败的事件（BPF 固定读 256B，越界 -EFAULT 会导致数据不完整）
        static unsigned long long parse_fail = 0;
        parse_fail++;
        if (parse_fail <= 20 || (parse_fail % 100) == 0) {
            char dump[96];
            int n = (size < 64) ? (int)size : 64;
            int p = 0;
            for (int i = 0; i < n && p < (int)sizeof(dump) - 4; i++) {
                unsigned char c = (unsigned char)str[i];
                dump[p++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            dump[p] = '\0';
            fprintf(stderr, "[ebpf_proxy] PARSE FAIL #%llu size=%zu head=[%s]\n",
                    parse_fail, size, dump);
        }
        return 0;
    }
    // 多通道：按 key 哈希选通道（同 key 恒定同通道 → 保序）
    int ch = 0;
    if (g_channels > 1 && key_len > 0) {
        ch = (int)(fnv1a(key, key_len) % (unsigned int)g_channels);
    }
    (void)channel_push(ch, str, cmd_len);
    return 0;
}

static void *monitor_thread_func(void *arg) {
    char **params = (char **)arg;

    for (int i = 0; i < g_channels; i++) channel_init(i);

    // 加载 eBPF 前先提升 memlock 限制
    raise_memlock_limit();

    // 过滤 libbpf 的 BTF 重试噪音日志
    libbpf_set_print(libbpf_print_filter);

    struct ebpf_monitor_bpf *skel = ebpf_monitor_bpf__open();
    if (!skel) goto cleanup_params;

    int err = ebpf_monitor_bpf__load(skel);
    if (err) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    err = ebpf_monitor_bpf__attach(skel);
    if (err) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    // 写入目标进程 tgid（kvserver）：BPF 只观察该进程的 write
    filter_tgid_map_fd = bpf_map__fd(skel->maps.filter_tgid);
    __u32 key = 0;
    __u32 tgid = (__u32)atoi(params[0]);
    if (bpf_map_update_elem(filter_tgid_map_fd, &key, &tgid, BPF_ANY) != 0) {
        fprintf(stderr, "[ebpf_proxy] WARN: filter_tgid map update failed: %s\n", strerror(errno));
    } else {
        printf("[ebpf_proxy] observing kvserver tgid=%u\n", tgid);
    }

    // ringbuf：全局共享一份（不再 per-CPU × 64MB），用户态靠超时轮询消费
    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.write_events),
                                             handle_event, NULL, NULL);
    if (!rb) { ebpf_monitor_bpf__destroy(skel); goto cleanup_params; }

    // 连接管理线程（1 个线程管 N 条连接，各自独立重试/探活）
    pthread_t conn_thread;
    pthread_create(&conn_thread, NULL, connect_thread_func, params);
    pthread_detach(conn_thread);

    // 回执排空线程：持续读走从库的 +OK 回执并丢弃
    // （从库 client_handler 每条命令都回复，不排空会把从库发送缓冲写满，
    //   导致其 flushReply 阻塞、整条同步链停摆 —— 必须先于 send 线程建立）
    pthread_t drain_thread;
    pthread_create(&drain_thread, NULL, drain_thread_func, NULL);
    pthread_detach(drain_thread);

    // 发送线程 × N：每个只写自己的连接、只取自己的队列
    for (int i = 0; i < g_channels; i++) {
        pthread_t t;
        pthread_create(&t, NULL, send_thread_func, (void *)(long)i);
        pthread_detach(t);
    }

    printf("[ebpf_proxy] forwarding to slave %s:%s via %d channel(s)\n",
           params[1], params[2], g_channels);

    // 调试统计：仅当环境变量 EBPF_STATS=1 时打印（默认关闭，避免刷屏）
    int stats_fd = bpf_map__fd(skel->maps.stats);
    const char *stats_env = getenv("EBPF_STATS");
    int show_stats = (stats_env && stats_env[0] == '1');
    unsigned long long last_stats[8] = {0};
    time_t last_stat_time = 0;

    unsigned long long rb_consumed = 0;
    while (!g_stop) {
        // BPF 侧用 BPF_RB_NO_WAKEUP 提交（不唤醒用户态，省掉每事件一次唤醒的开销），
        // 因此这里必须主动轮询消费：libbpf 的 ring_buffer__poll 依赖 epoll 事件，
        // 在 NO_WAKEUP 下永远收不到事件 → 必须用 ring_buffer__consume。
        err = ring_buffer__consume(rb);
        if (err < 0 && err != -EINTR) {
            static int poll_err_logged = 0;
            if (poll_err_logged++ < 5)
                fprintf(stderr, "[ebpf_proxy] ring_buffer__consume err=%d (%s)\n", err, strerror(-err));
            break;
        }
        if (err > 0) {
            rb_consumed += (unsigned long long)err;
        } else {
            usleep(200);   // 无数据：200μs 后再轮询（延迟上限 ≈200μs）
        }

        time_t now = time(NULL);
        if (show_stats && now != last_stat_time && stats_fd >= 0) {
            last_stat_time = now;
            unsigned long long v[8] = {0};
            int changed = 0;
            for (__u32 i = 0; i < 8; i++) {
                if (bpf_map_lookup_elem(stats_fd, &i, &v[i]) == 0 && v[i] != last_stats[i]) changed = 1;
            }
            // 队列速率诊断：push/pop 总数与各通道积压深度
            unsigned long long tot_push = 0, tot_pop = 0, depth[MAX_CHANNELS];
            for (int i = 0; i < g_channels; i++) {
                ChannelQueue *q = &g_q[i];
                pthread_mutex_lock(&q->lock);
                tot_push += q->push_cnt;
                tot_pop += q->pop_cnt;
                depth[i] = q->push_cnt - q->pop_cnt;
                pthread_mutex_unlock(&q->lock);
            }
            static unsigned long long last_push = 0, last_pop = 0;
            if (changed || tot_push != last_push || tot_pop != last_pop) {
                fprintf(stderr,
                        "[stats] enter=%llu tgid_ok=%llu len_ok=%llu fb_ok=%llu data_read_fail=%llu "
                        "rb_out_fail=%llu rb_consumed=%llu | push=%llu pop=%llu depth=[",
                        v[0], v[1], v[2], v[4], v[5], v[7], rb_consumed, tot_push, tot_pop);
                for (int i = 0; i < g_channels; i++) fprintf(stderr, "%s%llu", i ? "," : "", depth[i]);
                fprintf(stderr, "]\n");
                memcpy(last_stats, v, sizeof(last_stats));
                last_push = tot_push;
                last_pop = tot_pop;
            }
        }
    }

    // 退出前打印各通道残余积压（便于判断是否需要重新全量同步）
    for (int i = 0; i < g_channels; i++) {
        unsigned long long d = channel_depth(i);
        if (d > 0) {
            fprintf(stderr, "[ebpf_proxy] ch%d exiting with %llu pending commands\n", i, d);
        }
    }

    ring_buffer__free(rb);
    ebpf_monitor_bpf__destroy(skel);
    filter_tgid_map_fd = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (g_slave_fd[i] >= 0) close(g_slave_fd[i]);
        g_slave_fd[i] = -1;
    }

cleanup_params:
    free(params[0]);
    free(params[1]);
    free(params[2]);
    free(params);
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    // 日志立即落盘（重定向到文件时避免全缓冲导致诊断信息丢失）
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 3) {
        fprintf(stderr, "Usage: sudo %s <kvserver_pid> <从库IP> [端口=2000] [通道数]\n", argv[0]);
        fprintf(stderr, "  Example: sudo %s 12345 192.168.1.44 2000 4\n", argv[0]);
        fprintf(stderr, "  通道数优先取命令行第 4 参，否则取 kvstore.conf 的 ebpf_channels（默认 1）\n");
        return 1;
    }

    // 通道数：命令行 > kvstore.conf > 默认 1
    g_channels = conf_get_int("kvstore.conf", "ebpf_channels", 1);
    if (argc > 4) g_channels = atoi(argv[4]);
    if (g_channels < 1) g_channels = 1;
    if (g_channels > MAX_CHANNELS) g_channels = MAX_CHANNELS;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        g_slave_fd[i] = -1;
        g_pending_len[i] = 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char **params = (char **)malloc(3 * sizeof(char *));
    params[0] = strdup(argv[1]);   // kvserver pid（tgid）
    params[1] = strdup(argv[2]);   // 从库 IP
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", argc > 3 ? atoi(argv[3]) : 2000);
    params[2] = strdup(port_str);

    printf("[ebpf_proxy] starting (kvserver pid=%s, slave=%s:%s, channels=%d)\n",
           params[0], params[1], params[2], g_channels);

    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitor_thread_func, params);

    while (!g_stop) {
        sleep(1);
    }

    pthread_join(monitor_thread, NULL);
    free(params);
    printf("[ebpf_proxy] stopped\n");
    return 0;
}
