#include "rdma_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>

// 简易毫秒计时（诊断全量同步各阶段耗时用，stderr 无级别限制）
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
// 进度打印已按要求关闭（如需排查 RDMA 传输耗时，恢复下面原宏即可）
// 原宏：#define RDMA_TAG(fmt, ...) fprintf(stderr, "[rdma-t] " fmt "\n", ##__VA_ARGS__)
#define RDMA_TAG(fmt, ...) do { } while (0)

struct rdma_res {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    int buf_size;
    int sock_fd;
    uint32_t remote_qpn;
    uint16_t remote_lid;
    union ibv_gid remote_gid;
};

struct rdma_qp_info {
    uint32_t qpn;
    uint16_t lid;
    uint8_t gid[16];
};

// ===== 从库端 RDMA server 的监听 fd（可复用多次 accept）=====
static int g_listen_fd = -1;

// ==================== TCP 辅助（8 字节大端文件大小传输） ====================
static void put_be64(uint8_t *out, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(v >> (56 - i * 8));
    }
}

static uint64_t get_be64(const uint8_t *in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | in[i];
    }
    return v;
}

static int recv_full(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int send_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

// ==================== RDMA 基础设施 ====================

static int rdma_init(struct rdma_res *res, int buf_size) {
    memset(res, 0, sizeof(*res));
    res->buf_size = buf_size;
    res->sock_fd = -1;

    res->buf = (char *)malloc(buf_size);
    if (!res->buf) return -1;
    memset(res->buf, 0, buf_size);

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) return -1;

    res->ctx = ibv_open_device(dev_list[0]);
    if (!res->ctx) { ibv_free_device_list(dev_list); return -1; }
    ibv_free_device_list(dev_list);

    res->pd = ibv_alloc_pd(res->ctx);
    if (!res->pd) return -1;

    res->cq = ibv_create_cq(res->ctx, 10, NULL, NULL, 0);
    if (!res->cq) return -1;

    struct ibv_qp_init_attr qp_attr = {0};
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 4;
    qp_attr.cap.max_recv_wr = 4;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    if (!res->qp) return -1;

    res->mr = ibv_reg_mr(res->pd, res->buf, res->buf_size,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!res->mr) return -1;

    return 0;
}

// 动态查找 IPv4-mapped GID（::ffff:x.x.x.x）的 index，避免硬编码 index 1
// 重启后 rxe 的 GID 表顺序会变，index 1 可能被 IPv6 全局地址占用导致 RTR 失败
static int find_ipv4_gid_index(struct ibv_context *ctx, int port_num) {
    for (int i = 0; i < 256; i++) {
        union ibv_gid gid;
        if (ibv_query_gid(ctx, port_num, i, &gid) != 0)
            break;  // 超出有效 index
        int zero = 1;
        for (int j = 0; j < 10; j++) {
            if (gid.raw[j] != 0) { zero = 0; break; }
        }
        if (zero && gid.raw[10] == 0xff && gid.raw[11] == 0xff) {
            return i;
        }
    }
    return 1;  // 兜底：找不到就用 index 1
}

static int rdma_connect_qp(struct rdma_res *res) {
    struct ibv_qp_attr attr = {0};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    int ret = ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) return -1;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    // ===== 修改点：动态获取实际 MTU（rxe 在 1500 以太网上 active_mtu 是 1024）=====
    struct ibv_port_attr port_attr;
    if (ibv_query_port(res->ctx, 1, &port_attr) == 0) {
        attr.path_mtu = port_attr.active_mtu;
    } else {
        attr.path_mtu = IBV_MTU_1024;
    }
    attr.dest_qp_num = res->remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = res->remote_gid;
    attr.ah_attr.grh.sgid_index = find_ipv4_gid_index(res->ctx, 1);
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.dlid = res->remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    ret = ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) return -1;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) return -1;

    return 0;
}

static int rdma_exchange_qp_info(struct rdma_res *res, int is_server) {
    struct rdma_qp_info local_info, remote_info;

    local_info.qpn = htonl(res->qp->qp_num);
    struct ibv_port_attr port_attr;
    ibv_query_port(res->ctx, 1, &port_attr);
    local_info.lid = htons(port_attr.lid);
    union ibv_gid gid;
    int gid_idx = find_ipv4_gid_index(res->ctx, 1);
    ibv_query_gid(res->ctx, 1, gid_idx, &gid);
    memcpy(local_info.gid, &gid, 16);

    if (is_server) {
        if (recv_full(res->sock_fd, &remote_info, sizeof(remote_info)) != 0)
            return -1;
        if (send_full(res->sock_fd, &local_info, sizeof(local_info)) != 0)
            return -1;
    } else {
        if (send_full(res->sock_fd, &local_info, sizeof(local_info)) != 0)
            return -1;
        if (recv_full(res->sock_fd, &remote_info, sizeof(remote_info)) != 0)
            return -1;
    }

    res->remote_qpn = ntohl(remote_info.qpn);
    res->remote_lid = ntohs(remote_info.lid);
    memcpy(&res->remote_gid, remote_info.gid, 16);

    return 0;
}

static void rdma_cleanup(struct rdma_res *res) {
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
    if (res->sock_fd >= 0) close(res->sock_fd);
    free(res->buf);
    memset(res, 0, sizeof(*res));
}

// ==================== 阶段7：RDMA server（从库端） ====================
// 多 QP 并行传输（2026-09 改造）：单 QP 受 rxe 在飞窗口 + 虚拟网卡小包处理
// 限制（MTU 1024 时 ~25-30MB/s，多 QP 在 e1000 上 2 QP 即饱和）。
// 实测最优组合：VMXNET3 + jumbo(MTU 4096) + 8 QP ≈ 160MB/s（85MB 传输 0.53s）。
// 协议：1 条 ctrl TCP（8B 文件长 + 4B QP 数）→ N 条 QP TCP 各自交换 QP 信息
//       → N 线程并行收发分段（最后一段含余数）→ 每 QP 8B RDMA ack → ctrl ack。
#define RDMA_QP_MAX 16
#define RDMA_QP_COUNT 8

struct rdma_seg_job {
    struct rdma_res *res;
    int out_fd;    // server: pwrite 目标
    int file_fd;   // client: pread 源
    uint64_t offset;
    int is_server;
};

// 每 QP 一个线程：server 收段→落盘→ack；client 读段→发送→等 ack
static void *rdma_seg_thread(void *arg) {
    struct rdma_seg_job *job = (struct rdma_seg_job *)arg;
    struct rdma_res *res = job->res;
    int ret = 1;
    struct ibv_wc wc;

    if (job->is_server) {
        struct ibv_sge rsge = { (uintptr_t)res->buf, (uint32_t)res->buf_size, res->mr->lkey };
        struct ibv_recv_wr rwr = {0};
        rwr.wr_id = 1; rwr.sg_list = &rsge; rwr.num_sge = 1;
        struct ibv_recv_wr *bad_rwr;
        if (ibv_post_recv(res->qp, &rwr, &bad_rwr)) goto out;
        while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
        if (wc.status != IBV_WC_SUCCESS) goto out;
        int recv_len = wc.byte_len;
        if (pwrite(job->out_fd, res->buf, (size_t)recv_len, (off_t)job->offset) != recv_len) goto out;
        // 回 8B ack（数据已 pwrite 落盘，覆盖 buf 无碍）
        struct ibv_sge ssge = { (uintptr_t)res->buf, 8, res->mr->lkey };
        struct ibv_send_wr swr = {0};
        swr.wr_id = 2; swr.sg_list = &ssge; swr.num_sge = 1;
        swr.opcode = IBV_WR_SEND; swr.send_flags = IBV_SEND_SIGNALED;
        struct ibv_send_wr *bad_swr;
        if (ibv_post_send(res->qp, &swr, &bad_swr)) goto out;
        while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
        if (wc.status != IBV_WC_SUCCESS) goto out;
    } else {
        if (pread(job->file_fd, res->buf, (size_t)res->buf_size, (off_t)job->offset) != res->buf_size) goto out;
        struct ibv_sge rsge = { (uintptr_t)res->buf, 8, res->mr->lkey };
        struct ibv_recv_wr rwr = {0};
        rwr.wr_id = 1; rwr.sg_list = &rsge; rwr.num_sge = 1;
        struct ibv_recv_wr *bad_rwr;
        if (ibv_post_recv(res->qp, &rwr, &bad_rwr)) goto out;
        struct ibv_sge ssge = { (uintptr_t)res->buf, (uint32_t)res->buf_size, res->mr->lkey };
        struct ibv_send_wr swr = {0};
        swr.wr_id = 2; swr.sg_list = &ssge; swr.num_sge = 1;
        swr.opcode = IBV_WR_SEND; swr.send_flags = IBV_SEND_SIGNALED;
        struct ibv_send_wr *bad_swr;
        if (ibv_post_send(res->qp, &swr, &bad_swr)) goto out;
        for (int i = 0; i < 2; i++) {   // send 完成 + ack 完成
            while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
            if (wc.status != IBV_WC_SUCCESS) goto out;
        }
    }
    ret = 0;
out:
    return (void *)(long)ret;
}

int rdma_server_start(int port) {
    if (g_listen_fd >= 0) return 0;  // 已启动

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    g_listen_fd = fd;
    return 0;
}

int rdma_server_recv_file(int port, const char *out_path) {
    if (rdma_server_start(port) != 0) return -1;

    long t_start = now_ms();
    // 1. accept ctrl：8B 文件长 + 4B QP 数
    int cfd = accept(g_listen_fd, NULL, NULL);
    if (cfd < 0) return -1;
    uint8_t hdr[12];
    if (recv_full(cfd, hdr, 12) != 0) { close(cfd); return -1; }
    uint64_t file_size = get_be64(hdr);
    int n_qp = ((int)hdr[8] << 24) | ((int)hdr[9] << 16) | ((int)hdr[10] << 8) | hdr[11];
    if (file_size <= 0 || file_size > RDMA_MAX_FILE_SIZE) { close(cfd); return -1; }
    if (n_qp < 1 || n_qp > RDMA_QP_MAX) n_qp = RDMA_QP_COUNT;
    RDMA_TAG("server: file %llu B via %d QP", (unsigned long long)file_size, n_qp);

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { close(cfd); return -1; }

    // 2. accept N 条 QP TCP + 顺序握手（rdma_init 每 QP 独立注册段缓冲）
    struct rdma_res res[RDMA_QP_MAX];
    memset(res, 0, sizeof(res));
    uint64_t seg_base = file_size / (uint64_t)n_qp;
    int qps_ok = 0;
    for (int i = 0; i < n_qp; i++) {
        uint64_t off = seg_base * (uint64_t)i;
        int seg_len = (int)((i == n_qp - 1) ? (file_size - off) : seg_base);
        if (rdma_init(&res[i], seg_len) != 0) {
            fprintf(stderr, "[rdma] server rdma_init(%d) failed\n", i);
            break;
        }
        int s = accept(g_listen_fd, NULL, NULL);
        if (s < 0) { fprintf(stderr, "[rdma] server accept QP %d failed\n", i); break; }
        res[i].sock_fd = s;
        if (rdma_exchange_qp_info(&res[i], 1) != 0) break;
        if (rdma_connect_qp(&res[i]) != 0) break;
        qps_ok++;
    }
    if (qps_ok != n_qp) {
        fprintf(stderr, "[rdma] server handshake incomplete %d/%d\n", qps_ok, n_qp);
        for (int i = 0; i < RDMA_QP_MAX; i++) if (res[i].ctx) rdma_cleanup(&res[i]);
        close(cfd); close(out_fd);
        return -1;
    }
    RDMA_TAG("server handshake %d QP = %ld ms", n_qp, now_ms() - t_start);

    // 3. N 线程并行收段 → pwrite
    pthread_t th[RDMA_QP_MAX];
    struct rdma_seg_job jobs[RDMA_QP_MAX];
    for (int i = 0; i < n_qp; i++) {
        jobs[i].res = &res[i];
        jobs[i].out_fd = out_fd;
        jobs[i].file_fd = -1;
        jobs[i].offset = seg_base * (uint64_t)i;
        jobs[i].is_server = 1;
        pthread_create(&th[i], NULL, rdma_seg_thread, &jobs[i]);
    }
    int fail = 0;
    for (int i = 0; i < n_qp; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        if (ret) fail++;
    }
    RDMA_TAG("server RECV+write %lluMB done = %ld ms (fail=%d)",
             (unsigned long long)(file_size >> 20), now_ms() - t_start, fail);

    // 4. ctrl 完成 ack（全部落盘后）
    send_full(cfd, "1", 1);
    for (int i = 0; i < n_qp; i++) rdma_cleanup(&res[i]);
    close(cfd);
    close(out_fd);
    return fail ? -1 : (int)file_size;
}

// ==================== 阶段7：RDMA client（主库端） ====================
// 多 QP 并行发送（与 server 端配套，协议见 rdma_server_recv_file 注释）

int rdma_client_send_file(const char *slave_ip, int port, const char *file_path) {
    // 1. 打开文件
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        fprintf(stderr, "[rdma] open %s failed errno=%d\n", file_path, errno);
        return -1;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        fprintf(stderr, "[rdma] fstat failed errno=%d\n", errno);
        close(file_fd);
        return -1;
    }
    size_t len = (size_t)st.st_size;
    if (len <= 0 || len > RDMA_MAX_FILE_SIZE) {
        fprintf(stderr, "[rdma] file size invalid\n");
        close(file_fd);
        return -1;
    }
    int n_qp = RDMA_QP_COUNT;
    if (len < 1024 * 1024) n_qp = 1;   // 小文件单 QP 即可（省握手开销）

    long t_start = now_ms();
    // 2. ctrl TCP：8B 文件长 + 4B QP 数
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) { close(file_fd); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, slave_ip, &addr.sin_addr);
    if (connect(cfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[rdma] TCP connect to %s:%d failed errno=%d\n", slave_ip, port, errno);
        close(cfd); close(file_fd);
        return -1;
    }
    uint8_t hdr[12];
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)(len >> (56 - i * 8));
    hdr[8] = (uint8_t)(n_qp >> 24); hdr[9] = (uint8_t)(n_qp >> 16);
    hdr[10] = (uint8_t)(n_qp >> 8); hdr[11] = (uint8_t)n_qp;
    if (send_full(cfd, hdr, 12) != 0) { close(cfd); close(file_fd); return -1; }

    // 3. N 条 QP TCP + 顺序握手
    struct rdma_res res[RDMA_QP_MAX];
    memset(res, 0, sizeof(res));
    uint64_t seg_base = len / (uint64_t)n_qp;
    int qps_ok = 0;
    for (int i = 0; i < n_qp; i++) {
        uint64_t off = seg_base * (uint64_t)i;
        int seg_len = (int)((i == n_qp - 1) ? (len - off) : seg_base);
        if (rdma_init(&res[i], seg_len) != 0) {
            fprintf(stderr, "[rdma] client rdma_init(%d) failed\n", i);
            break;
        }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0 || connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "[rdma] QP TCP connect %d failed\n", i);
            if (s >= 0) close(s);
            break;
        }
        res[i].sock_fd = s;
        if (rdma_exchange_qp_info(&res[i], 0) != 0) break;
        if (rdma_connect_qp(&res[i]) != 0) break;
        qps_ok++;
    }
    if (qps_ok != n_qp) {
        fprintf(stderr, "[rdma] client handshake incomplete %d/%d\n", qps_ok, n_qp);
        for (int i = 0; i < RDMA_QP_MAX; i++) if (res[i].ctx) rdma_cleanup(&res[i]);
        close(cfd); close(file_fd);
        return -1;
    }
    RDMA_TAG("client handshake %d QP = %ld ms", n_qp, now_ms() - t_start);

    // 4. N 线程并行 pread 段 + SEND
    pthread_t th[RDMA_QP_MAX];
    struct rdma_seg_job jobs[RDMA_QP_MAX];
    for (int i = 0; i < n_qp; i++) {
        jobs[i].res = &res[i];
        jobs[i].out_fd = -1;
        jobs[i].file_fd = file_fd;
        jobs[i].offset = seg_base * (uint64_t)i;
        jobs[i].is_server = 0;
        pthread_create(&th[i], NULL, rdma_seg_thread, &jobs[i]);
    }
    int fail = 0;
    for (int i = 0; i < n_qp; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        if (ret) fail++;
    }
    RDMA_TAG("client SEND %lluMB done = %ld ms (fail=%d)",
             (unsigned long long)(len >> 20), now_ms() - t_start, fail);

    // 5. 等 ctrl 完成 ack（server 全部落盘后回）
    char done = 0;
    int ack_ok = (recv_full(cfd, &done, 1) == 0);
    RDMA_TAG("client ack = %ld ms; total send_file = %ld ms", now_ms() - t_start, now_ms() - t_start);

    for (int i = 0; i < n_qp; i++) rdma_cleanup(&res[i]);
    close(cfd);
    close(file_fd);
    return (fail || !ack_ok) ? -1 : 0;
}
