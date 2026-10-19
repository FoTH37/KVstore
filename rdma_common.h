#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define RDMA_PORT 2002
#define RDMA_BUFFER_SIZE (4 * 1024 * 1024)  // 4MB

struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    int buf_size;
    int sock_fd;
    uint32_t local_qpn;
    uint32_t remote_qpn;
    uint16_t local_lid;
    uint16_t remote_lid;
    union ibv_gid local_gid;
    union ibv_gid remote_gid;
};

struct rdma_qp_info {
    uint32_t qpn;
    uint16_t lid;
    uint8_t gid[16];
};

static int rdma_init(struct rdma_context *ctx, int buf_size) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->buf_size = buf_size;
    ctx->sock_fd = -1;

    ctx->buf = (char *)malloc(buf_size);
    if (!ctx->buf) return -1;
    memset(ctx->buf, 0, buf_size);
    return 0;
}

static int rdma_open_device(struct rdma_context *ctx) {
    // 获取可用设备列表
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list");
        return -1;
    }

    // 使用第一个 RDMA 设备
    ctx->ctx = ibv_open_device(dev_list[0]);
    if (!ctx->ctx) {
        perror("ibv_open_device");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("Using RDMA device: %s\n", ibv_get_device_name(dev_list[0]));
    ibv_free_device_list(dev_list);

    // 分配 PD
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (!ctx->pd) { perror("ibv_alloc_pd"); return -1; }

    // 创建 CQ
    ctx->cq = ibv_create_cq(ctx->ctx, 10, NULL, NULL, 0);
    if (!ctx->cq) { perror("ibv_create_cq"); return -1; }

    // 创建 QP
    struct ibv_qp_init_attr qp_attr = {0};
    qp_attr.send_cq = ctx->cq;
    qp_attr.recv_cq = ctx->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 4;
    qp_attr.cap.max_recv_wr = 4;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
    if (!ctx->qp) { perror("ibv_create_qp"); return -1; }

    ctx->local_qpn = ctx->qp->qp_num;
    printf("Local QPN: %u\n", ctx->local_qpn);

    // 获取本地 LID
    struct ibv_port_attr port_attr;
    ibv_query_port(ctx->ctx, 1, &port_attr);
    ctx->local_lid = port_attr.lid;
    printf("Local LID: %u\n", ctx->local_lid);

    // 获取本地 GID（使用索引 1，IPv4 映射地址，适用于回环）
    ibv_query_gid(ctx->ctx, 1, 1, &ctx->local_gid);

    // 注册内存
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->buf_size,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr) { perror("ibv_reg_mr"); return -1; }

    return 0;
}

static int rdma_create_tcp_connection(struct rdma_context *ctx, int is_server) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RDMA_PORT);

    if (is_server) {
        // 服务器端：监听
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) { perror("socket"); return -1; }
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
        listen(listen_fd, 1);
        printf("Listening on TCP port %d...\n", RDMA_PORT);
        ctx->sock_fd = accept(listen_fd, NULL, NULL);
        close(listen_fd);
        if (ctx->sock_fd < 0) { perror("accept"); return -1; }
        printf("TCP connection accepted.\n");
    } else {
        // 客户端：连接
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ctx->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctx->sock_fd < 0) { perror("socket"); return -1; }
        if (connect(ctx->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect"); return -1;
        }
        printf("TCP connection established.\n");
    }
    return 0;
}

static int rdma_exchange_qp_info(struct rdma_context *ctx) {
    struct rdma_qp_info local_info, remote_info;

    local_info.qpn = htonl(ctx->local_qpn);
    local_info.lid = htons(ctx->local_lid);
    memcpy(local_info.gid, &ctx->local_gid, 16);

    // 发送本地 QP 信息
    if (send(ctx->sock_fd, &local_info, sizeof(local_info), 0) != sizeof(local_info)) {
        perror("send"); return -1;
    }
    printf("Sent local QP info: QPN=%u LID=%u\n", ctx->local_qpn, ctx->local_lid);

    // 接收远程 QP 信息
    if (recv(ctx->sock_fd, &remote_info, sizeof(remote_info), 0) != sizeof(remote_info)) {
        perror("recv"); return -1;
    }

    ctx->remote_qpn = ntohl(remote_info.qpn);
    ctx->remote_lid = ntohs(remote_info.lid);
    memcpy(&ctx->remote_gid, remote_info.gid, 16);
    printf("Received remote QP info: QPN=%u LID=%u\n", ctx->remote_qpn, ctx->remote_lid);

    return 0;
}

static int rdma_connect_qp(struct rdma_context *ctx) {
    // 将 QP 从 RESET 转到 INIT
    struct ibv_qp_attr attr = {0};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    int ret = ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) { perror("ibv_modify_qp INIT"); return -1; }
    printf("QP moved to INIT.\n");

    // 从 INIT 转到 RTR（Ready to Receive）—— 使用全局路由
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = ctx->remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    // RXE 使用全局路由（GID），因为 LID=0
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = ctx->remote_gid;
    attr.ah_attr.grh.sgid_index = 1;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    ret = ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) { perror("ibv_modify_qp RTR"); return -1; }
    printf("QP moved to RTR.\n");

    // 从 RTR 转到 RTS（Ready to Send）
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) { perror("ibv_modify_qp RTS"); return -1; }
    printf("QP moved to RTS.\n");

    return 0;
}

static void rdma_cleanup(struct rdma_context *ctx) {
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ctx) ibv_close_device(ctx->ctx);
    if (ctx->sock_fd >= 0) close(ctx->sock_fd);
    free(ctx->buf);
    memset(ctx, 0, sizeof(*ctx));
}

#endif