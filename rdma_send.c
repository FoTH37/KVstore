#include "rdma_common.h"

int main() {
    printf("RDMA Slave (Sender): ready\n");

    struct rdma_context ctx;
    if (rdma_init(&ctx, RDMA_BUFFER_SIZE) != 0) {
        fprintf(stderr, "Failed to init RDMA context\n");
        return 1;
    }

    if (rdma_open_device(&ctx) != 0) return 1;

    // 用 sleep 确保接收端先运行
    sleep(1);

    if (rdma_create_tcp_connection(&ctx, 0) != 0) return 1;

    // 先发送本地的 QP 信息
    if (rdma_exchange_qp_info(&ctx) != 0) return 1;

    // 连接 QP
    if (rdma_connect_qp(&ctx) != 0) return 1;

    // 准备发送数据
    const char *test_data = "*3\r\n$3\r\nSET\r\n$4\r\nkey1\r\n$6\r\nvalue1\r\n";
    int data_len = strlen(test_data);
    memcpy(ctx.buf, test_data, data_len);

    // 提交 Send
    struct ibv_sge send_sge = {0};
    send_sge.addr = (uintptr_t)ctx.buf;
    send_sge.length = data_len;
    send_sge.lkey = ctx.mr->lkey;

    struct ibv_send_wr send_wr = {0};
    send_wr.wr_id = 2;
    send_wr.sg_list = &send_sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_send_wr;
    if (ibv_post_send(ctx.qp, &send_wr, &bad_send_wr)) {
        fprintf(stderr, "ibv_post_send failed\n");
        return 1;
    }

    printf("Sent %d bytes via RDMA: %s\n", data_len, test_data);

    // 等待发送完成
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx.cq, 1, &wc) == 0) {}
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Send WC status: %d\n", wc.status);
        return 1;
    }
    printf("RDMA send completed successfully.\n");

    rdma_cleanup(&ctx);
    printf("RDMA Slave done.\n");
    return 0;
}