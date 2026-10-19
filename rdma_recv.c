#include "rdma_common.h"

int main() {
    printf("RDMA Master (Receiver): ready on port %d\n", RDMA_PORT);

    struct rdma_context ctx;
    if (rdma_init(&ctx, RDMA_BUFFER_SIZE) != 0) {
        fprintf(stderr, "Failed to init RDMA context\n");
        return 1;
    }

    if (rdma_open_device(&ctx) != 0) return 1;
    if (rdma_create_tcp_connection(&ctx, 1) != 0) return 1;

    // 先接收对方的 QP 信息
    if (rdma_exchange_qp_info(&ctx) != 0) return 1;

    // 连接 QP
    if (rdma_connect_qp(&ctx) != 0) return 1;

    // 提交 Recv
    struct ibv_sge recv_sge = {0};
    recv_sge.addr = (uintptr_t)ctx.buf;
    recv_sge.length = ctx.buf_size;
    recv_sge.lkey = ctx.mr->lkey;

    struct ibv_recv_wr recv_wr = {0};
    recv_wr.wr_id = 1;
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;

    struct ibv_recv_wr *bad_recv_wr;
    if (ibv_post_recv(ctx.qp, &recv_wr, &bad_recv_wr)) {
        fprintf(stderr, "ibv_post_recv failed\n");
        return 1;
    }

    printf("Waiting for RDMA data...\n");

    // 等待接收完成
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx.cq, 1, &wc) == 0) {}
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Recv WC status: %d\n", wc.status);
        return 1;
    }

    printf("Received %d bytes via RDMA: %s\n", wc.byte_len, ctx.buf);

    rdma_cleanup(&ctx);
    printf("RDMA Master done.\n");
    return 0;
}