# KVStore — 多引擎 KV 存储服务器

基于 **ntyco 协程库** 的 KV 存储服务器：三种存储引擎运行时切换、AOF + RDB 混合持久化、**RDMA 全量同步 + eBPF 增量同步**的主从复制、多线程网络处理。

| 维度 | 实现 |
|---|---|
| 网络 | ntyco 协程（同步写法 / 异步执行）+ 多 worker 线程 + SO_REUSEPORT |
| 引擎 | HashTable（分片锁）/ RBTree / SkipList，运行时切换 |
| 持久化 | RDB 全量快照（mmap 加载）+ AOF 增量（io_uring + 分段 CRC32） |
| 主从同步 | RDMA 多 QP 全量（已有数据）+ eBPF 增量（实时命令）+ TCP 广播兜底 |
| 内存 | 自研 ObjectPool / jemalloc / glibc 三种分配方案对比 |

---

## 快速开始

```bash
make                                    # 编译（首次需 sudo apt install libmimalloc-dev）
./kvserver                              # 主库（默认端口 2000）

# 从库（另一台机器）
./kvslave 192.168.1.132                 # 连接主库触发全量同步，完成后提供 2000 只读查询

# eBPF 增量转发（主库，需 root）
sudo ./ebpf_proxy $(pgrep -x kvserver) 192.168.1.44 2000 4
```

**验证**：
```bash
redis-cli -p 2000 SET mykey hello       # 主库写入
redis-cli -p 2000 GET mykey             # 从库应读到 hello
```

---

## 配置（kvstore.conf）

| 项 | 默认 | 说明 |
|---|---|---|
| `port` | 2000 | 监听端口（主库读写 / 从库只读查询 / eBPF 转发接收 三合一） |
| `workers` | 6 | worker 线程数，建议 ≈ 核数 − 1 |
| `sync_senders` | 1 | 广播发送线程数，**必须为 1**（单条 TCP 流只能单写者） |
| `persist_mode` | both | rdb / aof / both |
| `loglevel` | warn | debug / info / warn / error |
| `rdma_slave_ip` / `rdma_slave_port` | — / 2003 | RDMA 全量同步目标 |
| `ebpf_channels` | 1 | eBPF 转发通道数（`hash(key)%N` 分片，同 key 保序） |
| `slave_workers` | 2 | 从库 worker 数（从库读） |

---

## 架构

```
客户端 ──RESP──▶ kvserver (workers × N, SO_REUSEPORT)
                    │
                    ├─ 引擎写入（HashTable 分片锁）
                    ├─ AOF 入队 ──▶ flushThread（攒批 + io_uring + fsync）
                    ├─ 广播入队 ──▶ sendLoop（锁外 write 到从库连接）
                    │                   │
                    │                   └──▶ eBPF 捕获 sys_enter_write
                    │                            │
                    └─ 回复客户端                 ▼
                                        ebpf_proxy（独立进程，root）
                                        RingBuf → 队列 → N 条 TCP 转发
                                                        │
                                                        ▼
                                             kvslave:2000（执行 + 回执）
```

**要点**
- **广播与 IO 分离**：worker 只入队（~0.1μs），网络写在独立线程且 **锁外** 执行
- **eBPF 数据源**：捕获 `sendLoop` 写给从库的 `write()`，业务代码零埋点
- **tgid 过滤**：只观察 kvserver 进程，天然消除转发进程的反馈环
- **一次 write = 一条命令**：eBPF 每个事件解析第一条完整 RESP 命令

---

## 主从同步

| 阶段 | 机制 | 说明 |
|---|---|---|
| **全量** | RDMA（多 QP=8 并行） | slave 发 SYNC → master SAVE → RDMA 分段传输 → slave 加载 → SYNC_FINISHED |
| **增量** | eBPF（RingBuf + 多通道转发） | 捕获广播 write → 按 key 哈希分通道 → 从库执行 |
| **兜底** | TCP 广播直连 | 从库 `receiveThread` 读取计数（数据丢弃），用于观测 |

**关键设计**
- **同一 key 保序**：`hash(key) % N` 分片 → 同一 sendLoop → 单条 TCP 有序字节流
- **RingBuf + `BPF_RB_NO_WAKEUP`**：BPF 提交不唤醒用户态，用户态 200μs 轮询消费 → 消除"每事件一次唤醒"的开销
- **无界队列 + chunk 池**：深积压不丢数据，malloc 次数从"每条一次"降到"每 4096 条一次"
- **回执排空线程**：从库对每条命令回 `+OK`，必须读走，否则从库发送缓冲写满导致整链停摆
- **阻塞 fd + 循环完整写**：避免部分写截断命令（eBPF 只解析第一条）

---

## 持久化

| 模式 | 机制 | 特点 |
|---|---|---|
| **RDB** | 全量快照，写 `.tmp` + 原子 rename | 裸 fd + thread_local 1MB 手写缓冲，零 per-key 分配 |
| **AOF** | RESP 追加，io_uring 批量写 + fsync | 分段 CRC32 校验，坏段跳过 |
| **恢复** | `loadSnapshot`（mmap）+ AOF 回放 | 先加载快照，再回放增量 |

**代价**：AOF 每秒批量刷盘，宕机最多丢 1 秒数据；全量快照成本 ∝ 库规模（扫描整个哈希表）。

---

## 性能测试

> **环境**：master 8 核 / 16GB ＋ slave 4 核 / 3.8GB，Ubuntu 24.04，VMXNET3 + MTU 9000，Soft-RoCE。
> 对比对象 Redis 7.0.15，压测客户端 redis-benchmark 7.0，服务端 `loglevel=warn`。
> **所有数据为本环境实测**（每项 3 次），环境特征见文末「环境瓶颈」。

### 1. 单连接 ECHO 基线

`redis-benchmark -c 1 -P 1 -n 100000 ECHO hello`（纯网络回显，不碰引擎）

| 场景 | 3 次 | 均值 |
|---|---|---|
| KVStore（persist_mode=rdb） | 1812 / 1806 / 1807 | **1808** |
| KVStore（persist_mode=both） | 1279 / 1443 / 1421 | **1381** |
| Redis（appendonly no） | 1579 / 1558 / 1581 | **1573** |
| Redis（appendonly yes） | 1577 / 1583 / 1579 | **1580** |

**结论**：rdb 模式与 Redis 持平（1808 vs 1573~1580）；both 模式因 AOF 入队开销略低（-12%）。

### 2. SAVE 频率对性能的影响

10 万条串行 SET，每 N 条触发一次 `SAVE`（全量快照），各 3 次：

| SAVE 频率 | 3 次 QPS | 均值 | 相对基线 |
|---|---|---|---|
| 100 万次/次（基线） | 1274 / 1495 / 1494 | **1421** | 100% |
| 10 万次/次 | 1453 / 1477 / 1465 | **1465** | 103% |
| 1 万次/次 | 1290 / 1326 / 1335 | **1317** | **-7%** |
| 1 千次/次 | 617 / 625 / 669 | **637** | **-55%** |

**单次 SAVE 成本**：10 万条触发 100 次 SAVE 耗时 162s（基线 67s）→ **每次 SAVE ≈ 0.95 秒**（库约 300 万 key，主要是遍历哈希表）。

**数学关系**：设单次快照成本 `T_save ∝ M`（M = key 数量），单条 SET 耗时 `t_set`，频率 `f`：
```
QPS_f / QPS_base = 1 / (1 + T_save / (f · t_set))
```
代入 `T_save=0.95s`、`f=1000`、`t_set=1/1421≈0.7ms`：`1/(1+0.95/(1000×0.0007)) = 1/2.36 ≈ 0.42`（实测 -55%，量级吻合）。

**结论**：全量快照成本由"扫描整个哈希表"决定，**无法通过实现优化消除**；高频持久化必须依赖 AOF 增量（成本只与本批数据量有关）。

### 3. Pipeline 批量对比

`redis-benchmark -c 1 -n 1000000 -t set -P N`（无主从同步），各 3 次取中位：

| P | KVStore（3 次） | 中位 | Redis（3 次） | 中位 | 比值 |
|---|---|---|---|---|---|
| 10 | 15855 / 15847 / 15890 | 15855 | 13476 / 13615 / 13622 | 13615 | **1.16x** |
| 20 | 31414 / 31500 / 31486 | 31486 | 20405 / 20271 / 20355 | 20355 | **1.55x** |
| 40 | 60857 / 61039 / 61181 | 61039 | 28123 / 28256 / 28378 | 28256 | **2.16x** |
| 80 | 117247 / 116754 / 116713 | 116754 | 34807 / 34380 / 34983 | 34807 | **3.35x** |
| 160 | 216357 / 220410 / 217628 | 217628 | 39140 / 38912 / 38900 | 38912 | **5.59x** |

**结论**：批量越大优势越明显（P160 领先 5.6 倍）—— 批量解析（游标 + 一次 erase）、批量回复（8KB 攒批）、引擎 O(1) 写入的综合效果。

### 4. 内存池对比

100 万次 64 字节分配/释放（**每次分配后 memset 写入并保留全部对象**，模拟真实场景），3 次：

| 分配器 | 耗时 | VIRT 峰值 | RSS 峰值 | RSS 结束 |
|---|---|---|---|---|
| **ObjectPool** | 267 / 265 / 264 ms → **265** | 150716 KB | **74560 KB** | 74560（不回落） |
| jemalloc | 312 / 296 / 313 ms → **312** | **103436 KB** | 77252 KB | 77360 |
| glibc malloc | 407 / 323 / 343 ms → **333** | 166056 KB | 89828 KB | 89956 |

**结论**：ObjectPool 最快（比 glibc 快 20%）且 RSS 最低；jemalloc VIRT 最优。glibc 两项均最差且释放后 RSS 不回落（碎片留在 arena）。

#### 分配耗时（ms，越低越好）

```
ObjectPool  ████████████████████████████████████████         265
jemalloc    ██████████████████████████████████████████████   312
glibc       ████████████████████████████████████████████████ 333
            └── 每格 ≈ 6.7 ms ──┘
```

#### 虚拟内存峰值 VIRT（KB，越低越好）

```
jemalloc    ███████████████████████████████                  103436
ObjectPool  █████████████████████████████████████████       150716
glibc       ██████████████████████████████████████████████  166056
            └── 每格 ≈ 3320 KB ──┘
```

#### 物理内存峰值 RSS（KB，越低越好）

```
ObjectPool  █████████████████████████████████████████       74560
jemalloc    ███████████████████████████████████████████     77252
glibc       ██████████████████████████████████████████████  89828
            └── 每格 ≈ 1800 KB ──┘
```

**三态特征（start → peak → end）**：ObjectPool 结束 RSS 不回落（池化常驻复用，以空间换时间）；jemalloc 结束略降（arena 归还部分内存）但 VIRT 持平；glibc 结束 RSS 微涨（释放的内存留在 arena 成为碎片）。

### 5. 主从实时同步（对主库写入的影响）

`redis-benchmark -c 50 -P 16 -n 1000000 -r 1000000 -t set`，每场景 **全新启动 kvserver + 预热 50 万条**，各 3 轮：

| 场景 | 轮 1 | 轮 2 | 轮 3 | 均值 | 相对无同步 |
|---|---|---|---|---|---|
| **无同步**（无从库连接） | 70269 | 69979 | 67259 | **69169** | 100% |
| **TCP 广播**（从库连接，eBPF 停） | 68951 | 68961 | 67723 | **68545** | **-0.9%** |
| **eBPF 转发**（完整链路） | 68889 | 69502 | 67870 | **68754** | **-0.6%** |

**结论**：本环境下同步开销 <1%。原因是压测速率（~6.9 万/s）低于广播链路的处理上限，广播入队与写出不构成瓶颈。

### 6. 全量传输（RDMA / sendfile / iperf3）

测试文件 `server_0_h.rdb` = **194,491,404 B（185 MB）**，传输时两端服务均停止：

| 方式 | 3 次耗时 | 吞吐 |
|---|---|---|
| **RDMA**（多 QP=8） | 2236 / 2231 / 2027 ms | **~87 MB/s** |
| **sendfile**（TCP 零拷贝） | 879 / 936 / 904 ms | **215 MB/s** |
| **iperf3**（字节模式，同数据量） | 3×1000 ms（每次 186 MB） | **194 MB/s**（1.55 Gbits/s） |
| **iperf3**（时间模式，链路能力） | 3.01 s 传 1.41 GB | **503 MB/s**（4.02 Gbits/s） |

**排序**：sendfile > iperf3 > RDMA。

**原因（硬件而非实现）**：当前使用 **Soft-RoCE（rxe）** —— 纯软件模拟 RDMA，无硬件卸载；内核模块自己构造 IB 包、UDP 封装、软件处理 ACK/PSN，且 MTU 受底层网卡档位限制（4096）。而 sendfile 走内核零拷贝 + jumbo 大段合并。

**真实硬件 RDMA（InfiniBand/RoCE 网卡）的预期关系相反**：RDMA > sendfile ≥ iperf3 —— 硬件旁路内核直接 DMA 用户态内存，带宽可打满线速、CPU 占用接近零、延迟低一个数量级。本项目的多 QP 分段传输、QP 信息交换、内存注册代码与真实硬件完全一致，换到真实 RoCE 网卡后同带宽下会反超 sendfile。

### 7. eBPF 增量转发性能

主库连续写入 10 万条 SET，观察 ebpf_proxy 与从库：

| 指标 | 数值 |
|---|---|
| 主库写入 10 万条 | 发送 0.11s / 全部回复 0.36s（≈28 万 cmd/s） |
| BPF 捕获（`fb_ok`） | **100000 / 100000** |
| 转发（push / pop） | **100000 / 100000**，队列深度 **0** |
| 从库抽样验证 | **7/7 通过**，无丢失 |

**RingBuf 优化前后对比**（同一环境，主库 write 速率）：

| 版本 | write 速率 | 每次 write | 说明 |
|---|---|---|---|
| perf buffer（原实现） | 8,372 /s | 119 μs | 每事件唤醒用户态 |
| **RingBuf + `BPF_RB_NO_WAKEUP`** | **31,810 /s** | **31 μs** | 变长提交 + 用户态主动轮询 |
| 无 eBPF（对照上限） | 31,243 /s | 32 μs | — |

**分阶段定位**（编译期开关，统计 4 秒 write 次数）：只过滤 +9μs / +读 256B +1μs / **+输出事件 +77μs（占总开销 80%）** → 瓶颈是"每事件一次用户态唤醒"，不是数据拷贝。

**多通道（1 vs 4）无收益**：瓶颈在主库单线程 sendLoop 的逐条 write（31.8k/s 上限），属上游限制。多通道代码保留（`ebpf_channels`，默认 1）。

### 8. perf 瓶颈分析

对 kvserver 在单连接 pipeline SET 负载下采样（`perf record -F 999 -g`）：

| 符号 | 占比 | 说明 |
|---|---|---|
| `delay_tsc` | **28.4%** | 内核时钟校准延迟循环 |
| `native_read_msr` | **24.3%** | 读 MSR（虚拟机中触发 vmexit） |
| `delay_tsc`（io_uring worker） | 10.7% | 同上 |
| `native_read_msr`（io_uring worker） | 9.1% | 同上 |
| `vmware_sched_clock` | 4.5% | VMware 半虚拟化调度时钟 |
| `read_tsc` / `native_write_msr` | 4.2% / 5.2% | 时间戳读写 |
| **`BusinessHandler::handleParsedCommand`** | **0.08%** | ← 业务处理 |
| **`HashTable::set`** | **0.04%** | ← 引擎写入 |

**结论**：~70% CPU 花在**读时钟**上（`native_read_msr` 在虚拟机里是 vmexit 操作），**业务代码占比不到 0.1%**。

**根因**：`dmesg` 显示 `[Firmware Bug]: TSC doesn't count with P0 frequency!` —— CPU 变频导致内核对 TSC 不信任，改用 `vmware_sched_clock`（hypercall 读取，极慢）。这解释了本环境所有网络往返测量偏慢约 4~5 倍（纯内核回环 TCP 往返 **107 μs**，正常应为 5~25 μs；Redis 同样受影响）。

**注意**：这是**虚拟化环境特性**，不是代码问题 —— 同一环境下 Redis 与 KVStore 受同等影响，两者的**相对对比有效**。

---

## 已知限制

1. **`sync_senders` 必须为 1**：广播出口是单条 TCP 连接，多线程并发阻塞写同一 socket 会因 sock 锁竞争导致主库 QPS 塌陷。
2. **eBPF 需要 root**：`ebpf_proxy` 独立进程加载 BPF 程序；kvserver 本身无需 root。
3. **命令长度 > 256B 不被同步**：BPF 侧固定读 256B（verifier 要求常量 size），大 value 命令静默不同步。
4. **ebpf_proxy 重启期间的写入无捕获**：需从库重新全量同步；进程内队列（无界）在崩溃时丢失。
5. **RBTree / SkipList 无锁**：多线程下仅 HashTable（分片锁）安全，`rset/sset` 建议单线程使用。
6. **AOF 定时刷盘**：每秒批量落盘，宕机最多丢 1 秒增量数据。
7. **rxe 设备重启后 GID 表顺序变化**：代码动态查找 IPv4-mapped GID；需确保 `rxe.service` 开机自启。
8. **ntyco 启动偶发段错误**：协程 sleep 定时器红黑树竞态（约 1/6~1/30 启动触发），重启即恢复。

---

## 常见问题

| 现象 | 原因 | 解决 |
|---|---|---|
| `rdma link show` 为空 | 重启后 rxe 设备丢失 | `sudo rdma link add rxe_ens160 type rxe netdev ens160`（或配 `rxe.service`） |
| `handleFullSync: rdma send failed` | 从库侧 RDMA 故障，或 RDB 超过 256MB 上限 | 查从库 `rdma link show` 与崩溃日志 |
| 从库数据不增长 | ebpf_proxy 未启动 / BPF 加载失败 | 以 root 启动 ebpf_proxy，观察 `Connected to slave` 日志 |
| 端口被占用 | 残留 kvserver/kvslave 进程 | `sudo fuser -k 2000/tcp 2003/tcp` |
| eBPF 转发错连僵尸连接 | 从库有残留进程占着 2000 | 测试前先清理两端进程 |
| `make clean` 后编译失败 | `vmlinux.h` / `ebpf_monitor.skel.h` 被删除 | `sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h` 后重新 `make` |

---

## 技术栈

ntyco（ucontext 协程）· SO_REUSEPORT 多 worker · HashTable/RBTree/SkipList · jemalloc/ObjectPool · io_uring（AOF）· mmap（RDB 加载）· libbpf + BPF RingBuf（eBPF 增量捕获）· libibverbs + Soft-RoCE（RDMA 多 QP 全量传输）· CRC32 分段校验 · RESP 协议
