CXX = g++
CC = gcc
# -MMD -MP：自动生成头文件依赖（.d 文件），改 .h 后 make 自动重编依赖者
# （曾因缺依赖跟踪：改 sync_manager.h 后 main.o 未重编，对象布局不匹配导致崩溃）
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -Wno-builtin-macro-redefined -Werror -fno-stack-clash-protection -MMD -MP
CFLAGS = -O2 -g -MMD -MP
# 阶段7-7：eBPF 已独立为 ebpf_proxy 进程，kvserver/kvslave 不再链接 libbpf
LDFLAGS_COMMON = -lpthread -luring -ldl -libverbs
LDFLAGS_EBPF = -lbpf -lelf -lz -lpthread

# ==================== ntyco 路径 ====================
NTYCO_DIR = thirdparty/ntyco
CPPFLAGS = -I$(NTYCO_DIR)/core
# ntyco C 文件编译时关闭所有警告
NTYCO_CFLAGS = $(CFLAGS) -w $(CPPFLAGS)

.PHONY: clean all

all: kvserver kvserver-mi kvserver-glibc benchmark_pipe insert10w verify10w kvslave insert5w_1 insert5w_2 bench_mempool benchmark_multi bench_mempool-glibc slave_verify10w slave_verify5w ebpf_proxy

# ==================== C++ 通用 .o 文件 ====================

main.o: main.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

persist.o: persist.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

BusinessHandler.o: BusinessHandler.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

config.o: config.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

sync_manager.o: sync_manager.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

hashtable.o: hashtable.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

rbtree.o: rbtree.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

skiplist.o: skiplist.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

ntynet.o: ntynet.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

ebpf_sync.o: ebpf_sync.c ebpf_monitor.skel.h
	$(CC) $(CFLAGS) -c $< -o $@

rdma_sync.o: rdma_sync.c rdma_sync.h
	$(CC) $(CFLAGS) -c $< -o $@

# ==================== ntyco 协程库 ====================

NTYCO_SRC = $(NTYCO_DIR)/core/nty_coroutine.c \
            $(NTYCO_DIR)/core/nty_schedule.c \
            $(NTYCO_DIR)/core/nty_socket.c \
            $(NTYCO_DIR)/core/nty_epoll.c

NTYCO_OBJ = $(NTYCO_SRC:.c=.o)

$(NTYCO_DIR)/core/%.o: $(NTYCO_DIR)/core/%.c
	$(CC) $(NTYCO_CFLAGS) -c $< -o $@

# ==================== 通用 .o 依赖列表 ====================

COMMON_OBJS = main.o persist.o BusinessHandler.o config.o sync_manager.o \
              hashtable.o rbtree.o skiplist.o ntynet.o rdma_sync.o \
              $(NTYCO_OBJ)

# ==================== 服务器各版本 ====================

kvserver: $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS_COMMON) -ljemalloc

kvserver-mi: $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS_COMMON) -lmimalloc

kvserver-glibc: $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS_COMMON)

# ==================== 测试工具 ====================

benchmark_pipe: benchmark_pipe.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

insert10w: insert10w.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

insert5w_1: insert5w_1.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

insert5w_2: insert5w_2.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

verify10w: verify10w.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

bench_mempool: bench_mempool.cc
	$(CXX) $(CXXFLAGS) -DJEMALLOC_LINKED -o $@ $< -lpthread -ljemalloc

bench_mempool-glibc: bench_mempool.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

benchmark_multi: benchmark_multi.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

slave_verify10w: slave_verify10w.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

slave_verify5w: slave_verify5w.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

# ==================== 从库（不链接 main.o）====================
# 注意：kvslave 必须先编 slave.o 再链接。若直接 "slave.cc -> kvslave"，
# -MMD 会生成 kvslave.d（内容 "kvslave: slave.cc 所有.h"），被 -include *.d
# 展开成目标依赖后，$^ 里混入 .h，g++ 会把 kvslave 误当成 precompiled header
# 输出（cc1plus --output-pch），二进制被写成 PCH 而损坏（Text file busy / data）。
slave.o: slave.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

kvslave: slave.o persist.o BusinessHandler.o config.o sync_manager.o \
         hashtable.o rbtree.o skiplist.o ntynet.o rdma_sync.o $(NTYCO_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS_COMMON)

# ==================== eBPF 测试 ====================

# 生成 vmlinux.h（需要 root，只生成一次）
vmlinux.h:
	sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# 编译 eBPF 内核程序（BPF 字节码）
ebpf_monitor.bpf.o: ebpf_monitor.bpf.c vmlinux.h
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -I. -c $< -o $@

# 生成 BPF skeleton（C 头文件）
ebpf_monitor.skel.h: ebpf_monitor.bpf.o
	bpftool gen skeleton $< > $@

# 编译用户态加载程序
ebpf_monitor: ebpf_monitor.c ebpf_monitor.skel.h
	$(CC) $(CFLAGS) -o $@ ebpf_monitor.c -lbpf -lelf -lz

# ==================== 阶段7-7：eBPF 独立转发进程 ====================

# eBPF 增量转发独立进程（需要 root 运行）：
#   sudo ./ebpf_proxy <kvserver_pid> <从库IP> [端口=2000]
ebpf_proxy: ebpf_proxy.c ebpf_monitor.skel.h
	$(CC) $(CFLAGS) -o $@ ebpf_proxy.c $(LDFLAGS_EBPF)

# ==================== RDMA 独立测试 ====================

rdma_recv: rdma_recv.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ rdma_recv.c -libverbs

rdma_send: rdma_send.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ rdma_send.c -libverbs

rdma_file_recv: rdma_file_recv.c rdma_sync.c rdma_sync.h
	$(CC) $(CFLAGS) -o $@ rdma_file_recv.c rdma_sync.c -libverbs

rdma_file_send: rdma_file_send.c rdma_sync.c rdma_sync.h
	$(CC) $(CFLAGS) -o $@ rdma_file_send.c rdma_sync.c -libverbs

# ==================== 清理 ====================

clean:
	rm -f kvserver kvserver-mi kvserver-glibc kvslave \
	      benchmark_pipe insert10w insert5w_1 insert5w_2 verify10w bench_mempool bench_mempool-glibc slave_verify10w slave_verify5w \
	      thirdparty/ntyco/core/*.o \
	      rdma_recv rdma_send ebpf_monitor ebpf_proxy ebpf_monitor.bpf.o ebpf_monitor.skel.h vmlinux.h \
	      rdma_sync.o \
	      *.o *.d server_*.aof server_*.rdb server.log

# ==================== 自动头文件依赖（-MMD -MP 生成的 .d） ====================
-include $(wildcard *.d)

