// mq_send.c —— 多 QP 并行 RDMA 文件发送原型（验证窗口并行收益）
// 用法: ./mq_send <ip> <port> <file> [qp_count=4]
// 对端: ./mq_recv <port> <out_file> [qp_count=4]
// 协议: 1 条 ctrl TCP(8B 文件长 + 4B QP 数) + N 条 QP TCP(各自交换 QP 信息)
//       数据面: N 个 QP 并行 SEND 文件分段; 每 QP 收 8B RDMA ack 完成
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <time.h>

#define MAX_QP 16

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

struct rdma_qp_info { uint32_t qpn; uint16_t lid; uint8_t gid[16]; };

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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

static int find_ipv4_gid_index(struct ibv_context *ctx, int port_num) {
    for (int i = 0; i < 256; i++) {
        union ibv_gid gid;
        if (ibv_query_gid(ctx, port_num, i, &gid) != 0) break;
        int zero = 1;
        for (int j = 0; j < 10; j++) if (gid.raw[j] != 0) { zero = 0; break; }
        if (zero && gid.raw[10] == 0xff && gid.raw[11] == 0xff) return i;
    }
    return 1;
}

// 每 QP 独立初始化（ctx/pd/cq/qp/mr 各自创建——原型先验证并行收益）
static int qp_init(struct rdma_res *res, int buf_size) {
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
    struct ibv_qp_init_attr qa = {0};
    qa.send_cq = res->cq;
    qa.recv_cq = res->cq;
    qa.qp_type = IBV_QPT_RC;
    qa.cap.max_send_wr = 4;
    qa.cap.max_recv_wr = 4;
    qa.cap.max_send_sge = 1;
    qa.cap.max_recv_sge = 1;
    res->qp = ibv_create_qp(res->pd, &qa);
    if (!res->qp) return -1;
    res->mr = ibv_reg_mr(res->pd, res->buf, res->buf_size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!res->mr) return -1;
    return 0;
}

static int qp_connect(struct rdma_res *res) {
    struct ibv_qp_attr attr = {0};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) return -1;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    struct ibv_port_attr pa;
    attr.path_mtu = (ibv_query_port(res->ctx, 1, &pa) == 0) ? pa.active_mtu : IBV_MTU_1024;
    attr.dest_qp_num = res->remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = res->remote_gid;
    attr.ah_attr.grh.sgid_index = find_ipv4_gid_index(res->ctx, 1);
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.dlid = res->remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;
    if (ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) return -1;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(res->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) return -1;
    return 0;
}

static int qp_exchange(struct rdma_res *res) {
    struct rdma_qp_info local_info, remote_info;
    local_info.qpn = htonl(res->qp->qp_num);
    struct ibv_port_attr pa;
    ibv_query_port(res->ctx, 1, &pa);
    local_info.lid = htons(pa.lid);
    union ibv_gid gid;
    ibv_query_gid(res->ctx, 1, find_ipv4_gid_index(res->ctx, 1), &gid);
    memcpy(local_info.gid, &gid, 16);
    if (send_full(res->sock_fd, &local_info, sizeof(local_info))) return -1;
    if (recv_full(res->sock_fd, &remote_info, sizeof(remote_info))) return -1;
    res->remote_qpn = ntohl(remote_info.qpn);
    res->remote_lid = ntohs(remote_info.lid);
    memcpy(&res->remote_gid, remote_info.gid, 16);
    return 0;
}

static void qp_cleanup(struct rdma_res *res) {
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
    if (res->sock_fd >= 0) close(res->sock_fd);
    free(res->buf);
    memset(res, 0, sizeof(*res));
}

// ===== 每 QP 数据线程：读段 → SEND → 等 ack =====
struct seg_arg {
    struct rdma_res *res;
    int file_fd;
    uint64_t offset;
};

static void *send_seg_thread(void *arg) {
    struct seg_arg *a = (struct seg_arg *)arg;
    struct rdma_res *res = a->res;

    ssize_t rd = pread(a->file_fd, res->buf, res->buf_size, (off_t)a->offset);
    if (rd != res->buf_size) {
        fprintf(stderr, "[mq] seg@%llu pread %zd/%d\n", (unsigned long long)a->offset, rd, res->buf_size);
        return (void *)1;
    }

    // post recv(8B ack) —— ack 在对端收完全段后才到，不会覆盖未发送数据
    struct ibv_sge rsge = { (uintptr_t)res->buf, 8, res->mr->lkey };
    struct ibv_recv_wr rwr = {0};
    rwr.wr_id = 1;
    rwr.sg_list = &rsge;
    rwr.num_sge = 1;
    struct ibv_recv_wr *bad_rwr;
    if (ibv_post_recv(res->qp, &rwr, &bad_rwr)) { fprintf(stderr, "[mq] post_recv ack failed\n"); return (void *)1; }

    // post send(全段)
    struct ibv_sge ssge = { (uintptr_t)res->buf, (uint32_t)res->buf_size, res->mr->lkey };
    struct ibv_send_wr swr = {0};
    swr.wr_id = 2;
    swr.sg_list = &ssge;
    swr.num_sge = 1;
    swr.opcode = IBV_WR_SEND;
    swr.send_flags = IBV_SEND_SIGNALED;
    struct ibv_send_wr *bad_swr;
    if (ibv_post_send(res->qp, &swr, &bad_swr)) { fprintf(stderr, "[mq] post_send failed\n"); return (void *)1; }

    // 等 send + ack 两个完成
    struct ibv_wc wc;
    for (int i = 0; i < 2; i++) {
        while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[mq] CQ error status=%d\n", wc.status);
            return (void *)1;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <ip> <port> <file> [qp_count=4]\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    const char *file = argv[3];
    int qp_count = (argc > 4) ? atoi(argv[4]) : 4;
    if (qp_count < 1 || qp_count > MAX_QP) qp_count = 4;

    int file_fd = open(file, O_RDONLY);
    if (file_fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(file_fd, &st);
    uint64_t len = (uint64_t)st.st_size;
    printf("[mq] send %s (%llu B) via %d QP to %s:%d\n", file,
           (unsigned long long)len, qp_count, ip, port);

    // 1. ctrl TCP: 文件长(8B) + QP 数(4B)
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(cfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("ctrl connect"); return 1; }
    uint8_t hdr[12];
    for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)(len >> (56 - i * 8));
    hdr[8] = (uint8_t)(qp_count >> 24); hdr[9] = (uint8_t)(qp_count >> 16);
    hdr[10] = (uint8_t)(qp_count >> 8); hdr[11] = (uint8_t)qp_count;
    if (send_full(cfd, hdr, 12)) { perror("send hdr"); return 1; }

    // 2. N 条 QP TCP + 顺序握手
    struct rdma_res res[MAX_QP];
    uint64_t seg_base = len / (uint64_t)qp_count;
    long t0 = now_ms();
    for (int i = 0; i < qp_count; i++) {
        uint64_t off = seg_base * (uint64_t)i;
        int seg_len = (int)((i == qp_count - 1) ? (len - off) : seg_base);
        if (qp_init(&res[i], seg_len)) { fprintf(stderr, "[mq] qp_init %d failed\n", i); return 1; }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("qp connect"); return 1; }
        res[i].sock_fd = s;
        if (qp_exchange(&res[i])) { fprintf(stderr, "[mq] exchange %d failed\n", i); return 1; }
        if (qp_connect(&res[i])) { fprintf(stderr, "[mq] qp_connect %d failed\n", i); return 1; }
    }
    long t1 = now_ms();
    printf("[mq] handshake %d QP = %ld ms\n", qp_count, t1 - t0);

    // 3. N 线程并行发送
    pthread_t th[MAX_QP];
    struct seg_arg args[MAX_QP];
    for (int i = 0; i < qp_count; i++) {
        args[i].res = &res[i];
        args[i].file_fd = file_fd;
        args[i].offset = seg_base * (uint64_t)i;
        pthread_create(&th[i], NULL, send_seg_thread, &args[i]);
    }
    long t2 = now_ms();
    int fail = 0;
    for (int i = 0; i < qp_count; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        if (ret) fail++;
    }
    long t3 = now_ms();
    printf("[mq] transfer = %ld ms (%.1f MB/s), failures=%d\n", t3 - t2,
           (double)len / (t3 - t2) / 1000.0, fail);

    // 4. 收 ctrl 完成 ack（server 全部落盘后回）
    char done;
    if (recv_full(cfd, &done, 1) != 0) fprintf(stderr, "[mq] ctrl done ack failed\n");
    long t4 = now_ms();
    printf("[mq] TOTAL = %ld ms (含落盘确认; 传输+握手 %.1f MB/s)\n", t4 - t0,
           (double)len / (t4 - t0) / 1000.0);

    for (int i = 0; i < qp_count; i++) qp_cleanup(&res[i]);
    close(cfd);
    close(file_fd);
    return fail ? 1 : 0;
}
