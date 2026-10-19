#!/bin/bash
# 测试不同 SAVE 频率对 QPS 的影响
# 用法：./test_save_qps.sh
# 测 5 档：每 1 次 / 每 1k 次 / 每 1w 次 / 每 10w 次 / 每 100w 次 save

PORT=2000
REQUESTS=100000  # 每档总请求数

# 清理
pkill -9 kvserver 2>/dev/null
rm -f server_0_*.aof server_0_*.rdb
sleep 1

# 确保 both 模式（SAVE 才生效）
sed -i 's/persist_mode=.*/persist_mode=both/' kvstore.conf

# 启动服务器
sudo ./kvserver &
sleep 3

echo "=== SAVE 频率对 QPS 的影响测试 ==="
echo "端口: $PORT, 每档请求数: $REQUESTS"
echo ""

# 辅助函数：按频率发 SAVE
# 参数1: 每次 save 间隔的请求数
run_test() {
    local save_every=$1
    local label=$2

    # 启动一个后台进程，周期性发 SAVE
    (
        # 计算 SAVE 周期（秒），约每 save_every 条发一次
        # 用估算：每秒约 4000 条（单连接），所以间隔 = save_every / 4000 秒
        interval=$(awk "BEGIN {printf \"%.3f\", $save_every / 4000}")
        while true; do
            sleep $interval
            redis-cli -p $PORT SAVE > /dev/null 2>&1
        done
    ) &
    SAVE_PID=$!

    # 跑 redis-benchmark
    echo "--- 每 $label 次 save 一次 ---"
    redis-benchmark -p $PORT -c 1 -n $REQUESTS set k v 2>&1 | grep "requests per second"

    # 停掉 save 后台进程
    kill $SAVE_PID 2>/dev/null
    wait $SAVE_PID 2>/dev/null
    echo ""
}

# 测 5 档
run_test 1       "1次"
run_test 1000    "1k次"
run_test 10000   "1w次"
run_test 100000  "10w次"
run_test 1000000 "100w次"

# 停服务器
pkill -9 kvserver 2>/dev/null
echo "=== 测试完成 ==="