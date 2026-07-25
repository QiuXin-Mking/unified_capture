#!/bin/bash
# unified_capture socket 验收测试
# 用法: chmod +x test_socket.sh && ./test_socket.sh

SOCK="/tmp/unified_capture.sock"

echo "=== 1. 测试 status (初始化中) ==="
echo "status" | nc -U "$SOCK" 2>/dev/null || echo "FAIL: socket 不可达"

echo ""
echo "=== 2. 等待 ready ==="
for i in $(seq 1 30); do
    resp=$(echo "status" | nc -U "$SOCK" 2>/dev/null)
    if echo "$resp" | grep -q '"ready":true'; then
        echo "设备就绪: $resp"
        break
    fi
    echo "等待... ($i/30)"
    sleep 1
done

echo ""
echo "=== 3. 测试 start ==="
START_RESP=$(echo "start" | nc -U "$SOCK" 2>/dev/null)
echo "start 响应: $START_RESP"

echo ""
echo "=== 4. 测试 status (采集中) ==="
sleep 0.5
STATUS_RESP=$(echo "status" | nc -U "$SOCK" 2>/dev/null)
echo "status 响应: $STATUS_RESP"

echo ""
echo "=== 5. 测试重复 start (应返回 already running) ==="
DUP_RESP=$(echo "start" | nc -U "$SOCK" 2>/dev/null)
echo "重复 start: $DUP_RESP"

echo ""
echo "=== 6. 测试 stop ==="
STOP_RESP=$(echo "stop" | nc -U "$SOCK" 2>/dev/null)
echo "stop 响应: $STOP_RESP"

echo ""
echo "=== 7. 测试重复 stop (应返回 not running) ==="
sleep 0.5
DUP_STOP=$(echo "stop" | nc -U "$SOCK" 2>/dev/null)
echo "重复 stop: $DUP_STOP"

echo ""
echo "=== 8. 测试未知命令 ==="
UNK_RESP=$(echo "reboot" | nc -U "$SOCK" 2>/dev/null)
echo "未知命令: $UNK_RESP"

echo ""
echo "=== 9. 测试 final status (空闲) ==="
FINAL=$(echo "status" | nc -U "$SOCK" 2>/dev/null)
echo "空闲: $FINAL"

echo ""
echo "=== 所有测试完成 ==="
