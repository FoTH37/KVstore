#!/bin/bash
# 一键运行所有测试：持久化 + 主从同步 + 性能回归
# 用法：./run_all_tests.sh [perf_threshold]

cd "$(dirname "$0")"

THRESHOLD=${1:-3000}
PASS_COUNT=0
FAIL_COUNT=0

echo "========================================"
echo " KVStore 全量测试套件"
echo "========================================"

echo ""
echo "########## 测试 1: 持久化 (rdb/aof/both) ##########"
if ./test_persist.sh; then
    PASS_COUNT=$((PASS_COUNT+1))
    echo "[PASS] 持久化测试"
else
    FAIL_COUNT=$((FAIL_COUNT+1))
    echo "[FAIL] 持久化测试"
fi

echo ""
echo "########## 测试 2: 主从同步 (RDMA+eBPF) ##########"
if ./test_sync.sh; then
    PASS_COUNT=$((PASS_COUNT+1))
    echo "[PASS] 主从同步测试"
else
    FAIL_COUNT=$((FAIL_COUNT+1))
    echo "[FAIL] 主从同步测试"
fi

echo ""
echo "########## 测试 3: 性能回归 ##########"
if ./test_perf.sh $THRESHOLD; then
    PASS_COUNT=$((PASS_COUNT+1))
    echo "[PASS] 性能回归测试"
else
    FAIL_COUNT=$((FAIL_COUNT+1))
    echo "[FAIL] 性能回归测试"
fi

echo ""
echo "========================================"
echo " 结果: $PASS_COUNT 通过, $FAIL_COUNT 失败"
echo "========================================"

exit $FAIL_COUNT