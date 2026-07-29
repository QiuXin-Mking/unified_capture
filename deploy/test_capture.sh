#!/usr/bin/env bash
#
# test_capture.sh — 一键构建 + 启动采集测试
#
# 用法 (在 deploy/ 目录下运行):
#   ./test_capture.sh                   构建 + 采集 30 秒
#   ./test_capture.sh -d 60             采集 60 秒
#   ./test_capture.sh build             仅构建 (不采集)
#   ./test_capture.sh status            查看设备状态
#   ./test_capture.sh stop              停止正在运行的采集
#
# 输出目录: /media/usb0/capture/t/<timestamp>/session_001/
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"

BOARD="root@192.168.100.200"
REMOTE_SRC="/root/unified_capture"
BINARY="/usr/local/bin/unified_capture"

# ── 颜色 ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERR]${NC} $*"; }

# ── sync ──
do_sync() {
    info "同步源码: $LOCAL_SRC → $BOARD:$REMOTE_SRC/"

    # 先清理板端旧源码 (避免目录嵌套问题)
    ssh "$BOARD" "cd $REMOTE_SRC && rm -rf app core hardware docs tests \
        CLAUDE.md CMakeLists.txt Makefile README.md unified_capture 2>/dev/null; true"

    # rsync 同步: 源目录内容 → 板端目标
    rsync -avz --delete \
        --exclude='build/' \
        --exclude='deploy/' \
        --exclude='.git/' \
        --exclude='tools/' \
        --exclude='*.o' \
        "$LOCAL_SRC/" \
        "$BOARD:$REMOTE_SRC/"

    info "同步完成"
}

# ── build ──
do_build() {
    info "板端编译..."
    ssh "$BOARD" "cd $REMOTE_SRC && make clean && make -j\$(nproc)" 2>&1 | tail -20
    info "安装到 $BINARY"
    ssh "$BOARD" "cp $REMOTE_SRC/unified_capture $BINARY"
}

# ── status ──
do_status() {
    info "=== 设备列表 ==="
    ssh "$BOARD" "v4l2-ctl --list-devices 2>/dev/null" || true
    echo ""
    info "=== 运行中的采集 ==="
    ssh "$BOARD" "ps aux | grep unified_capture | grep -v grep" || echo "(无)"
}

# ── stop ──
do_stop() {
    info "停止采集..."
    ssh "$BOARD" "pkill -TERM unified_capture 2>/dev/null" || true
    sleep 1
    ssh "$BOARD" "pgrep unified_capture >/dev/null && echo '仍在运行' || echo '已停止'"
}

# ── capture ──
do_capture() {
    local duration="${1:-30}"
    local ts
    ts=$(date +%Y%m%d_%H%M%S)
    local prefix="t_${ts}"

    info "采集 ${duration}s, 前缀=$prefix"

    # 先停旧的
    ssh "$BOARD" "pkill -TERM unified_capture 2>/dev/null" || true
    sleep 1

    info "启动..."
    ssh "$BOARD" "timeout ${duration} $BINARY --no-gpio $prefix 2>&1" || {
        local rc=$?
        if [ $rc -eq 124 ]; then
            info "采集完成 (timeout ${duration}s)"
        elif [ $rc -eq 143 ]; then
            info "收到 SIGTERM, 正常退出"
        elif [ $rc -eq 137 ]; then
            info "被 SIGKILL, 可能超时"
        else
            warn "退出码=$rc (可能是部分设备缺失导致提前退出)"
        fi
    }

    local outdir="/media/usb0/capture/${prefix}"
    info "输出: $outdir"
    ssh "$BOARD" "ls -lhR $outdir 2>/dev/null || echo '(目录为空或未创建)'"
}

# ── main ──
case "${1:-}" in
    build)
        do_sync
        do_build
        ;;
    status)
        do_status
        ;;
    stop)
        do_stop
        ;;
    -d)
        do_sync
        do_build
        do_capture "${2:-30}"
        ;;
    *)
        do_sync
        do_build
        do_capture "${1:-30}"
        ;;
esac
