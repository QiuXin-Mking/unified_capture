#!/usr/bin/env bash
#
# Repeatedly measure whether JHH02 MJPEG and JHH04 YUYV can stream together.
# Intended to run on the RK3588 board as root.
#
#   ./test_yuyv_concurrency.sh              # 3 rounds, 120 frames each
#   ./test_yuyv_concurrency.sh -n 10 -c 300
#   ./test_yuyv_concurrency.sh --no-service # do not stop systemd first
#
set -euo pipefail

JHH02_DEVICE="/dev/video0"
JHH04_DEVICE="/dev/video6"
SERVICE="unified_capture"
ROUNDS=3
COUNT=120
KEEP=0
MANAGE_SERVICE=1

usage() {
    sed -n '1,14p' "$0"
}

while (($# > 0)); do
    case "$1" in
        -n|--rounds)
            ROUNDS="$2"
            shift 2
            ;;
        -c|--count)
            COUNT="$2"
            shift 2
            ;;
        --jhh02)
            JHH02_DEVICE="$2"
            shift 2
            ;;
        --jhh04)
            JHH04_DEVICE="$2"
            shift 2
            ;;
        --service)
            SERVICE="$2"
            shift 2
            ;;
        --keep)
            KEEP=1
            shift
            ;;
        --no-service)
            MANAGE_SERVICE=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ "$ROUNDS" =~ ^[1-9][0-9]*$ ]] || { echo "rounds must be positive" >&2; exit 2; }
[[ "$COUNT" =~ ^[1-9][0-9]*$ ]] || { echo "count must be positive" >&2; exit 2; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required" >&2; exit 2; }

TEST_DIR="$(mktemp -d /tmp/yuyv-concurrency.XXXXXX)"
RESTORE_SERVICE=0

cleanup() {
    local rc=$?
    if ((RESTORE_SERVICE)); then
        systemctl start "$SERVICE" >/dev/null || {
            echo "WARN: failed to restart $SERVICE" >&2
            rc=1
        }
    fi
    if ((KEEP)); then
        echo "Artifacts kept at $TEST_DIR"
    else
        rm -f "$TEST_DIR"/*
        rmdir "$TEST_DIR"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

if ((MANAGE_SERVICE)) && command -v systemctl >/dev/null &&
   systemctl is-active --quiet "$SERVICE"; then
    echo "Stopping $SERVICE to release V4L2 devices"
    systemctl stop "$SERVICE"
    RESTORE_SERVICE=1
fi

run_stream() {
    local name="$1" device="$2" format="$3" width="$4" height="$5"
    local output="$TEST_DIR/${name}.raw" log="$TEST_DIR/${name}.log"
    rm -f "$output" "$log"
    v4l2-ctl -d "$device" \
        --set-fmt-video="width=${width},height=${height},pixelformat=${format}" \
        --stream-mmap=4 --stream-count="$COUNT" \
        --stream-to="$output" --verbose >"$log" 2>&1
}

last_sequence() {
    local log="$1"
    sed -n 's/.* seq:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$log" | tail -1
}

last_error() {
    local log="$1"
    grep -E 'failed:|returned -1|No space left|Device or resource' "$log" |
        tail -1 || true
}

has_stream_error() {
    local log="$1"
    grep -Eq 'VIDIOC_(S_FMT|REQBUFS|STREAMON) returned -1|failed:|No space left|Device or resource' "$log"
}

report() {
    local name rc log
    name="$1"
    rc="$2"
    log="$TEST_DIR/${name}.log"
    local bytes=0 sequence="-" error="-" ok="yes"
    [[ -f "$TEST_DIR/${name}.raw" ]] &&
        bytes="$(stat -c '%s' "$TEST_DIR/${name}.raw")"
    sequence="$(last_sequence "$log")"
    [[ -n "$sequence" ]] || sequence="-"
    error="$(last_error "$log")"
    [[ -n "$error" ]] || error="-"
    if ((rc != 0)) || has_stream_error "$log"; then
        ok="no"
    fi
    printf 'RESULT name=%s ok=%s rc=%s bytes=%s last_sequence=%s error=%s\n' \
        "$name" "$ok" "$rc" "$bytes" "$sequence" "$error"
}

run_single() {
    local name="$1" device="$2" format="$3" width="$4" height="$5"
    set +e
    run_stream "$name" "$device" "$format" "$width" "$height"
    local rc=$?
    set -e
    if has_stream_error "$TEST_DIR/${name}.log"; then
        rc=1
    fi
    report "$name" "$rc"
    return "$rc"
}

run_pair() {
    local name="$1" first="$2" second="$3"
    local first_pid second_pid first_rc second_rc
    rm -f "$TEST_DIR/${name}_"*.raw "$TEST_DIR/${name}_"*.log

    set +e
    if [[ "$first" == "jhh02" ]]; then
        run_stream "${name}_jhh02" "$JHH02_DEVICE" MJPG 4000 1200 &
        first_pid=$!
    else
        run_stream "${name}_jhh04" "$JHH04_DEVICE" YUYV 3104 480 &
        first_pid=$!
    fi
    sleep 1
    if [[ "$second" == "jhh02" ]]; then
        run_stream "${name}_jhh02" "$JHH02_DEVICE" MJPG 4000 1200 &
        second_pid=$!
    else
        run_stream "${name}_jhh04" "$JHH04_DEVICE" YUYV 3104 480 &
        second_pid=$!
    fi
    wait "$first_pid"
    first_rc=$?
    wait "$second_pid"
    second_rc=$?
    set -e

    report "${name}_jhh02" "$([[ "$first" == jhh02 ]] && echo "$first_rc" || echo "$second_rc")"
    report "${name}_jhh04" "$([[ "$first" == jhh04 ]] && echo "$first_rc" || echo "$second_rc")"
}

echo "Testing JHH02=$JHH02_DEVICE MJPG 4000x1200 and JHH04=$JHH04_DEVICE YUYV 3104x480"
echo "rounds=$ROUNDS frames_per_stream=$COUNT"

overall_rc=0
for ((round = 1; round <= ROUNDS; ++round)); do
    echo "--- round $round/$ROUNDS: single streams ---"
    run_single "r${round}_jhh02_single" "$JHH02_DEVICE" MJPG 4000 1200 || overall_rc=1
    run_single "r${round}_jhh04_single" "$JHH04_DEVICE" YUYV 3104 480 || overall_rc=1

    echo "--- round $round/$ROUNDS: concurrent, jhh02 first ---"
    run_pair "r${round}_jhh02_first" jhh02 jhh04
    echo "--- round $round/$ROUNDS: concurrent, jhh04 first ---"
    run_pair "r${round}_jhh04_first" jhh04 jhh02
done

echo "Single-stream failures: $overall_rc"
echo "Concurrent results are diagnostic; inspect ok= and error= (v4l2-ctl may return rc=0 after STREAMON failure)."
exit "$overall_rc"
