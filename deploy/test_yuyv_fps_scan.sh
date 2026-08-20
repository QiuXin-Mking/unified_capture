#!/usr/bin/env bash
#
# 逐档扫描 JHH04 YUYV 的帧率，找出能与 JHH02 MJPG 同时启流的最高帧率。
#
# 背景：JHH02 MJPG(4000x1200) 与 JHH04 YUYV(3104x480) 各自单流正常，
#       但并发启流时后启动的一路 STREAMON 失败（带宽不足 / ENOSPC）。
#       YUYV 是未压缩格式，带宽 ≈ 宽×高×2×fps，降帧率可线性降低带宽占用。
#       本脚本逐档设帧率并测并发，输出「可并发启流的最高帧率」。
#
# 用法：
#   ./test_yuyv_fps_scan.sh                    # 默认扫描 30/20/15/10/5/2
#   ./test_yuyv_fps_scan.sh --fps "30 20 15 10 8 5 3 1"
#   ./test_yuyv_fps_scan.sh --list             # 仅列出 JHH04 YUYV 支持的帧率，不测试
#   ./test_yuyv_fps_scan.sh --no-service       # 不先停 systemd 服务
#   ./test_yuyv_fps_scan.sh -c 60              # 每路抓 60 帧（更快，但不那么稳）
#
# 说明：设备可能不接受请求的帧率值（驱动会就近调整），脚本每次都会用
#       --get-parm 读回「实际生效帧率」并据此记录，而不是记录请求值。
#
set -euo pipefail

JHH02_DEVICE="/dev/video0"
JHH04_DEVICE="/dev/video6"
SERVICE="unified_capture"
COUNT=120
FPS_LIST="30 20 15 10 5 2"
MJPG_FPS=30
MANAGE_SERVICE=1
LIST_ONLY=0

usage() {
    sed -n '2,20p' "$0"
}

while (($# > 0)); do
    case "$1" in
        -c|--count)
            COUNT="$2"; shift 2 ;;
        --fps)
            FPS_LIST="$2"; shift 2 ;;
        --mjpg-fps)
            MJPG_FPS="$2"; shift 2 ;;
        --jhh02)
            JHH02_DEVICE="$2"; shift 2 ;;
        --jhh04)
            JHH04_DEVICE="$2"; shift 2 ;;
        --service)
            SERVICE="$2"; shift 2 ;;
        --list)
            LIST_ONLY=1; shift ;;
        --no-service)
            MANAGE_SERVICE=0; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required" >&2; exit 2; }

if ((LIST_ONLY)); then
    echo "=== JHH04 ($JHH04_DEVICE) YUYV 支持的格式与帧率 ==="
    v4l2-ctl -d "$JHH04_DEVICE" --list-formats-ext | grep -A 30 -i "YUYV" || \
        v4l2-ctl -d "$JHH04_DEVICE" --list-formats-ext
    echo
    echo "=== JHH02 ($JHH02_DEVICE) MJPG 支持的格式与帧率 ==="
    v4l2-ctl -d "$JHH02_DEVICE" --list-formats-ext | grep -A 30 -i "MJPG" || \
        v4l2-ctl -d "$JHH02_DEVICE" --list-formats-ext
    exit 0
fi

# 帧率列表按从高到低排序（脚本按此顺序测，第一个能并发成功的就是最高帧率）
FPS_LIST="$(echo "$FPS_LIST" | tr -s ' ' '\n' | sort -rn | tr '\n' ' ')"

TEST_DIR="$(mktemp -d /tmp/yuyv-fps-scan.XXXXXX)"
RESTORE_SERVICE=0

cleanup() {
    local rc=$?
    if ((RESTORE_SERVICE)); then
        systemctl start "$SERVICE" >/dev/null || {
            echo "WARN: failed to restart $SERVICE" >&2; rc=1
        }
    fi
    rm -f "$TEST_DIR"/*
    rmdir "$TEST_DIR" 2>/dev/null || true
    exit "$rc"
}
trap cleanup EXIT INT TERM

if ((MANAGE_SERVICE)) && command -v systemctl >/dev/null &&
   systemctl is-active --quiet "$SERVICE"; then
    echo "Stopping $SERVICE to release V4L2 devices"
    systemctl stop "$SERVICE"
    RESTORE_SERVICE=1
fi

# 读回设备实际生效的帧率
get_fps() {
    local device="$1"
    v4l2-ctl -d "$device" --get-parm 2>/dev/null |
        sed -n 's/.*Frames per second:[[:space:]]*\([0-9.]*\).*/\1/p' | head -1
}

# 设帧率并回读。返回 0 表示设置成功（读到非空值）
set_fps() {
    local device="$1" fps="$2"
    v4l2-ctl -d "$device" --set-parm="$fps" >/dev/null 2>&1 || true
    get_fps "$device"
}

# 单路抓流。第 6 个参数为可选帧率（为空则不设）
run_stream() {
    local name="$1" device="$2" format="$3" width="$4" height="$5" fps="$6"
    local output="$TEST_DIR/${name}.raw" log="$TEST_DIR/${name}.log"
    rm -f "$output" "$log"
    local parm_args=()
    [[ -n "$fps" ]] && parm_args=(--set-parm="$fps")
    v4l2-ctl -d "$device" \
        --set-fmt-video="width=${width},height=${height},pixelformat=${format}" \
        "${parm_args[@]}" \
        --stream-mmap=4 --stream-count="$COUNT" \
        --stream-to="$output" --verbose >"$log" 2>&1
}

last_error() {
    local log="$1"
    grep -E 'failed:|returned -1|No space left|Device or resource|VIDIOC_' "$log" |
        tail -1 || true
}

has_stream_error() {
    local log="$1"
    grep -Eq 'VIDIOC_(S_FMT|REQBUFS|STREAMON) returned -1|failed:|No space left|Device or resource' "$log"
}

report() {
    local name rc log
    name="$1"; rc="$2"; log="$TEST_DIR/${name}.log"
    local bytes=0 error="-" ok="yes"
    [[ -f "$TEST_DIR/${name}.raw" ]] &&
        bytes="$(stat -c '%s' "$TEST_DIR/${name}.raw")"
    error="$(last_error "$log")"
    [[ -n "$error" ]] || error="-"
    if ((rc != 0)) || has_stream_error "$log"; then
        ok="no"
    fi
    printf '  %-28s ok=%-3s rc=%s bytes=%s error=%s\n' \
        "$name" "$ok" "$rc" "$bytes" "$error"
}

# 单路测试 JHH04（验证该帧率本身能跑）
test_jhh04_single() {
    local fps="$1" effective
    effective="$(set_fps "$JHH04_DEVICE" "$fps")"
    local name="jhh04_single_fps${effective}"
    set +e
    run_stream "$name" "$JHH04_DEVICE" YUYV 3104 480 "$fps"
    local rc=$?
    set -e
    has_stream_error "$TEST_DIR/${name}.log" && rc=1
    echo "  [单路 JHH04 YUYV @ 请求${fps}fps 实际${effective}fps]"
    report "$name" "$rc"
    return "$rc"
}

# 并发测试：JHH02 MJPG 先启，1s 后 JHH04 YUYV（降帧率）
test_concurrent() {
    local fps="$1" effective
    effective="$(set_fps "$JHH04_DEVICE" "$fps")"
    local jhh02_name="jhh02_mjpg_fps${MJPG_FPS}"
    local jhh04_name="jhh04_yuyv_fps${effective}"
    rm -f "$TEST_DIR/${jhh02_name}"* "$TEST_DIR/${jhh04_name}"*

    set +e
    run_stream "$jhh02_name" "$JHH02_DEVICE" MJPG 4000 1200 "$MJPG_FPS" &
    local mjpg_pid=$!
    sleep 1
    run_stream "$jhh04_name" "$JHH04_DEVICE" YUYV 3104 480 "$fps" &
    local yuyv_pid=$!
    wait "$mjpg_pid"; local mjpg_rc=$?
    wait "$yuyv_pid"; local yuyv_rc=$?
    set -e

    has_stream_error "$TEST_DIR/${jhh02_name}.log" && mjpg_rc=1
    has_stream_error "$TEST_DIR/${jhh04_name}.log" && yuyv_rc=1

    echo "  [并发 JHH02 MJPG@${MJPG_FPS}fps + JHH04 YUYV@请求${fps}fps 实际${effective}fps]"
    report "$jhh02_name" "$mjpg_rc"
    report "$jhh04_name" "$yuyv_rc"
    # 两路都成功才算并发成功
    [[ "$mjpg_rc" -eq 0 && "$yuyv_rc" -eq 0 ]]
}

echo "=== 设备 ==="
echo "JHH02=$JHH02_DEVICE MJPG 4000x1200 @${MJPG_FPS}fps"
echo "JHH04=$JHH04_DEVICE YUYV 3104x480  帧率扫描: $FPS_LIST"
echo "frames_per_stream=$COUNT"
echo

# 先确认 JHH02 MJPG 单独能跑（后续并发判断的基线）
echo "=== 基线：JHH02 MJPG 单路 ==="
mjpg_base_ok=1
run_stream "jhh02_mjpg_base" "$JHH02_DEVICE" MJPG 4000 1200 "$MJPG_FPS"
mjpg_base_rc=$?
has_stream_error "$TEST_DIR/jhh02_mjpg_base.log" && mjpg_base_rc=1
report "jhh02_mjpg_base" "$mjpg_base_rc"
[[ "$mjpg_base_rc" -eq 0 ]] && mjpg_base_ok=0
echo

echo "=== 逐档扫描 ==="
best_fps="-"
for fps in $FPS_LIST; do
    echo "--- 帧率 $fps fps ---"

    if ! test_jhh04_single "$fps"; then
        echo "  → JHH04 单路在此帧率下失败，跳过并发测试"
        echo
        continue
    fi

    if test_concurrent "$fps"; then
        echo "  → ✅ 并发成功"
        best_fps="$fps"
    else
        echo "  → ❌ 并发失败"
    fi
    echo
done

echo "=== 结论 ==="
if [[ "$best_fps" == "-" ]]; then
    echo "所有扫描帧率都无法并发启流。"
    echo "建议：--list 查看设备支持的更低帧率，或检查是否走 USB2.0 hub（带宽 480Mbps 硬上限）。"
else
    echo "可并发启流的最高（请求）帧率：${best_fps} fps"
    echo "注意：实际生效帧率以每档 [实际xxxfps] 标注为准，设备可能就近调整。"
fi
