#!/bin/bash
# 测试三种持久化模式：rdb / aof / both
# 每种模式：插入10w → 杀进程 → 重启 → verify10w → 检查 PASS/FAIL

set -e
cd "$(dirname "$0")"

PASS_COUNT=0
FAIL_COUNT=0

test_mode() {
    local mode=$1
    echo "=== 测试 persist_mode=$mode ==="

    # 清理
    pkill -9 kvserver 2>/dev/null || true
    rm -f server_0_*.aof server_0_*.rdb dump.rdb
    sleep 1

    # 设置配置
    sed -i "s/^persist_mode=.*/persist_mode=$mode/" kvstore.conf

    # 启动服务器
    ./kvserver &
    SERVER_PID=$!
    # 等待服务器就绪
    sleep 2

    # 插入 10 万条
    ./insert10w

    # 如果是 rdb 模式，需要手动 SAVE
    if [ "$mode" = "rdb" ]; then
        redis-cli -p 2000 SAVE > /dev/null 2>&1
        sleep 1
    else
        sleep 10  # 等 AOF 刷盘
    fi

    # 杀掉服务器
    kill -9 $SERVER_PID 2>/dev/null || true
    pkill -9 kvserver 2>/dev/null || true
    sleep 1

    # 重启
    ./kvserver &
    SERVER_PID=$!
    # ===== 关键修改：等待 15 秒，确保服务器完成 AOF/RDB 文件加载 =====
    sleep 15

    # 验证
    if ./verify10w | grep -q "Status: PASS"; then
        echo "  [PASS] $mode 模式"
        PASS_COUNT=$((PASS_COUNT+1))
    else
        echo "  [FAIL] $mode 模式"
        FAIL_COUNT=$((FAIL_COUNT+1))
    fi

    # 清理
    kill -9 $SERVER_PID 2>/dev/null || true
    pkill -9 kvserver 2>/dev/null || true
    sleep 1
}

# 恢复默认配置
trap 'sed -i "s/^persist_mode=.*/persist_mode=both/" kvstore.conf' EXIT

test_mode "rdb"
test_mode "aof"
test_mode "both"

echo ""
echo "===== 结果: $PASS_COUNT 通过, $FAIL_COUNT 失败 ====="
exit $FAIL_COUNT