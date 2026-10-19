#!/bin/bash
# 性能回归测试：使用 benchmark_multi（多连接并发）测量 QPS
# 阈值低于预期则视为性能回归
# 用法：./test_perf.sh [阈值]  （默认 20000）

cd "$(dirname "$0")"

# 性能阈值（QPS 低于此值视为回归，默认 20000，留安全余量）
THRESHOLD=${1:-20000}
CONCURRENCY=${2:-8}

echo "=== 性能回归测试 ==="
echo "  并发连接数: $CONCURRENCY"
echo "  阈值: QPS < $THRESHOLD 视为回归"

# 清理并启动服务器
pkill -9 kvserver 2>/dev/null || true
rm -f server_0_*.aof server_0_*.rdb
sleep 1

./kvserver > /tmp/kvserver_perf.log 2>&1 &
SERVER_PID=$!
# 等待服务器就绪（AOF/RDB 恢复需要时间）
sleep 3

# 检查服务器是否起来（用 redis-cli PING 确认就绪）
if ! redis-cli -p 2000 PING > /dev/null 2>&1; then
    # 服务器可能还在恢复，多等几秒重试
    sleep 5
    if ! redis-cli -p 2000 PING > /dev/null 2>&1; then
        echo "  [FAIL] 服务器启动失败"
        cat /tmp/kvserver_perf.log
        kill -9 $SERVER_PID 2>/dev/null
        exit 1
    fi
fi
echo "  [OK] 服务器已启动"

# 运行 benchmark_multi（多连接并发压测）
echo "  运行 benchmark_multi $CONCURRENCY..."
./benchmark_multi $CONCURRENCY > /tmp/perf_result.log 2>&1

# 提取 QPS
QPS=$(grep "QPS" /tmp/perf_result.log | awk '{print $2}')

if [ -z "$QPS" ]; then
    echo "  [FAIL] 无法获取 QPS"
    cat /tmp/perf_result.log
    kill -9 $SERVER_PID 2>/dev/null
    exit 1
fi

echo "  QPS: $QPS"

# 清理服务器
kill -9 $SERVER_PID 2>/dev/null || true
pkill -9 kvserver 2>/dev/null || true

# 阈值判断（QPS 可能是整数或浮点）
if awk "BEGIN {exit !($QPS >= $THRESHOLD)}"; then
    echo "  [PASS] QPS $QPS >= 阈值 $THRESHOLD"
    exit 0
else
    echo "  [FAIL] QPS $QPS < 阈值 $THRESHOLD，性能回归！"
    exit 1
fi