// mq_recv.c —— 多 QP 并行 RDMA 文件接收原型
// 用法: ./mq_recv <port> <out_file> [qp_count=4]
// 先起本端，发送端 ./mq_send <ip> <port> <file> <qp_count> 随后连接
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
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

static int qp_exchange_server(struct rdma_res *res) {
    struct rdma_qp_info local_info, remote_info;
    local_info.qpn = htonl(res->qp->qp_num);
    struct ibv_port_attr pa;
    ibv_query_port(res->ctx, 1, &pa);
    local_info.lid = htons(pa.lid);
    union ibv_gid gid;
    ibv_query_gid(res->ctx, 1, find_ipv4_gid_index(res->ctx, 1), &gid);
    memcpy(local_info.gid, &gid, 16);
    if (recv_full(res->sock_fd, &remote_info, sizeof(remote_info))) return -1;
    if (send_full(res->sock_fd, &local_info, sizeof(local_info))) return -1;
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

// ===== 每 QP 数据线程：RECV 段 → 落盘 → RDMA ack =====
struct seg_arg {
    struct rdma_res *res;
    int out_fd;
    uint64_t offset;
};

static void *recv_seg_thread(void *arg) {
    struct seg_arg *a = (struct seg_arg *)arg;
    struct rdma_res *res = a->res;

    struct ibv_sge rsge = { (uintptr_t)res->buf, (uint32_t)res->buf_size, res->mr->lkey };
    struct ibv_recv_wr rwr = {0};
    rwr.wr_id = 1;
    rwr.sg_list = &rsge;
    rwr.num_sge = 1;
    struct ibv_recv_wr *bad_rwr;
    if (ibv_post_recv(res->qp, &rwr, &bad_rwr)) { fprintf(stderr, "[mq] post_recv failed\n"); return (void *)1; }

    struct ibv_wc wc;
    while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[mq] recv CQ error status=%d\n", wc.status);
        return (void *)1;
    }
    int recv_len = wc.byte_len;

    ssize_t w = pwrite(a->out_fd, res->buf, recv_len, (off_t)a->offset);
    if (w != recv_len) { fprintf(stderr, "[mq] pwrite %zd/%d\n", w, recv_len); return (void *)1; }

    // 回 8B ack（发送完成才到对端，覆盖 buf 无碍——数据已 pwrite 落盘）
    struct ibv_sge ssge = { (uintptr_t)res->buf, 8, res->mr->lkey };
    struct ibv_send_wr swr = {0};
    swr.wr_id = 2;
    swr.sg_list = &ssge;
    swr.num_sge = 1;
    swr.opcode = IBV_WR_SEND;
    swr.send_flags = IBV_SEND_SIGNALED;
    struct ibv_send_wr *bad_swr;
    if (ibv_post_send(res->qp, &swr, &bad_swr)) { fprintf(stderr, "[mq] ack send failed\n"); return (void *)1; }
    while (ibv_poll_cq(res->cq, 1, &wc) == 0) usleep(500);
    if (wc.status != IBV_WC_SUCCESS) { fprintf(stderr, "[mq] ack CQ error\n"); return (void *)1; }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <out_file> [qp_count=4]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *outFile = argv[2];
    int qp_count = (argc > 3) ? atoi(argv[3]) : 4;
    if (qp_count < 1 || qp_count > MAX_QP) qp_count = 4;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    printf("[mq] recv listening %d -> %s (%d QP)\n", port, outFile, qp_count);

    long t0 = now_ms();
    // 1. accept ctrl: 文件长(8B) + QP 数(4B)
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { perror("accept ctrl"); return 1; }
    uint8_t hdr[12];
    if (recv_full(cfd, hdr, 12)) { fprintf(stderr, "[mq] read hdr failed\n"); return 1; }
    uint64_t len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | hdr[i];
    int n_qp = (hdr[8] << 24) | (hdr[9] << 16) | (hdr[10] << 8) | hdr[11];
    if (n_qp < 1 || n_qp > MAX_QP) n_qp = qp_count;
    printf("[mq] file %llu B via %d QP\n", (unsigned long long)len, n_qp);

    int ofd = open(outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 1; }

    // 2. accept N 条 QP TCP + 顺序握手
    struct rdma_res res[MAX_QP];
    uint64_t seg_base = len / (uint64_t)n_qp;
    long t1 = now_ms();
    for (int i = 0; i < n_qp; i++) {
        uint64_t off = seg_base * (uint64_t)i;
        int seg_len = (int)((i == n_qp - 1) ? (len - off) : seg_base);
        if (qp_init(&res[i], seg_len)) { fprintf(stderr, "[mq] qp_init %d failed\n", i); return 1; }
        int s = accept(lfd, NULL, NULL);
        if (s < 0) { perror("accept qp"); return 1; }
        res[i].sock_fd = s;
        if (qp_exchange_server(&res[i])) { fprintf(stderr, "[mq] exchange %d failed\n", i); return 1; }
        if (qp_connect(&res[i])) { fprintf(stderr, "[mq] qp_connect %d failed\n", i); return 1; }
    }
    long t2 = now_ms();
    printf("[mq] handshake %d QP = %ld ms\n", n_qp, t2 - t1);

    // 3. N 线程并行接收
    pthread_t th[MAX_QP];
    struct seg_arg args[MAX_QP];
    for (int i = 0; i < n_qp; i++) {
        args[i].res = &res[i];
        args[i].out_fd = ofd;
        args[i].offset = seg_base * (uint64_t)i;
        pthread_create(&th[i], NULL, recv_seg_thread, &args[i]);
    }
    int fail = 0;
    for (int i = 0; i < n_qp; i++) {
        void *ret = NULL;
        pthread_join(th[i], &ret);
        if (ret) fail++;
    }
    long t3 = now_ms();
    printf("[mq] RECV+write = %ld ms (%.1f MB/s), failures=%d\n", t3 - t2,
           (double)len / (t3 - t2) / 1000.0, fail);

    // 4. ctrl 完成 ack
    if (send_full(cfd, "1", 1)) fprintf(stderr, "[mq] done ack failed\n");
    long t4 = now_ms();
    printf("[mq] TOTAL = %ld ms\n", t4 - t0);

    for (int i = 0; i < n_qp; i++) qp_cleanup(&res[i]);
    close(cfd);
    close(ofd);
    close(lfd);
    return fail ? 1 : 0;
}
