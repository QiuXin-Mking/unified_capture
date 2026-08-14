#!/usr/bin/env bash
# RK3588-only acceptance for the mango wrist profile.
#
# Start unified_capture with a fresh, timestamped PREFIX before each case.
# This script never deletes capture data.  Disconnect cameras between runs and
# select the expected inventory with CASE=two, CASE=one, or CASE=zero.
set -euo pipefail

: "${PREFIX:?set PREFIX to the fresh capture output directory}"
: "${SOCK:?set SOCK to the unified_capture Unix socket}"

CASE="${CASE:-two}"
case "$CASE" in
    two|one|zero) ;;
    *) echo "CASE must be two, one, or zero" >&2; exit 2 ;;
esac

send() {
    printf '%s\n' "$1" | nc -U "$SOCK"
}

require() {
    local value=$1 expected=$2
    printf '%s\n' "$value" | grep -Fq "$expected" || {
        echo "missing expected response field: $expected" >&2
        printf 'response: %s\n' "$value" >&2
        exit 1
    }
}

wait_running() {
    local expected=$1 status
    for _ in $(seq 1 100); do
        status=$(send status)
        if printf '%s\n' "$status" | grep -Fq "\"running\":$expected"; then
            return 0
        fi
        sleep 0.1
    done
    echo "timed out waiting for running=$expected" >&2
    exit 1
}

assert_outputs() {
    local session_dir camera expect_imu camera_dir
    session_dir=$1
    camera=$2
    expect_imu=${3:-true}
    camera_dir="$session_dir/$camera"
    local mkvs=() imus=()
    shopt -s nullglob
    mkvs=("$camera_dir"/*.mkv)
    imus=("$camera_dir"/*.jsonl)
    shopt -u nullglob
    ((${#mkvs[@]} > 0)) || { echo "missing $camera MKV" >&2; exit 1; }
    for mkv in "${mkvs[@]}"; do
        test "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
            -of default=noprint_wrappers=1:nokey=1 "$mkv")" = hevc || {
            echo "expected H.265/HEVC MKV: $mkv" >&2; exit 1;
        }
    done
    if $expect_imu; then
        ((${#imus[@]} > 0)) || { echo "missing $camera IMU JSONL" >&2; exit 1; }
        for imu in "${imus[@]}"; do
            test -s "$imu" || { echo "empty IMU JSONL: $imu" >&2; exit 1; }
        done
    fi
}

assert_no_capture_files() {
    local session_dir=$1
    if find "$session_dir" -type f \( -name '*.mkv' -o -name '*.jsonl' -o -name '*.y8' \) -print -quit | grep -q .; then
        echo "zero-device session unexpectedly wrote capture files" >&2
        exit 1
    fi
}

status=$(send status)
require "$status" '"product":"mango"'
require "$status" '"wrist_left"'
require "$status" '"wrist_right"'
require "$status" '"ready":true'
require "$status" '"imu":true'

case "$CASE" in
    two)
        require "$status" '"degraded":false'
        require "$status" '"wrist_left":true'
        require "$status" '"wrist_right":true'
        ;;
    one)
        require "$status" '"degraded":true'
        ;;
    zero)
        require "$status" '"degraded":true'
        require "$status" '"wrist_left":false'
        require "$status" '"wrist_right":false'
        ;;
esac

require "$(send start)" '"ok":true'
wait_running true

# Keep the sample short but long enough to produce encoded video and IMU data.
sleep "${CAPTURE_SECONDS:-3}"
require "$(send stop)" '"ok":true'
wait_running false

session_dir=$(find "$PREFIX" -mindepth 1 -maxdepth 1 -type d -name 'session_*' -print | sort | tail -n 1)
test -n "$session_dir" || { echo "no session directory below $PREFIX" >&2; exit 1; }
if find "$session_dir" -name '*.y8' -print -quit | grep -q .; then
    echo "mango must not write Y8 files" >&2
    exit 1
fi

case "$CASE" in
    two)
        assert_outputs "$session_dir" wrist_left
        assert_outputs "$session_dir" wrist_right
        ;;
    one)
        left=$(find "$session_dir/wrist_left" -type f -name '*.mkv' -print -quit 2>/dev/null || true)
        right=$(find "$session_dir/wrist_right" -type f -name '*.mkv' -print -quit 2>/dev/null || true)
        if test -n "$left" && test -z "$right"; then
            assert_outputs "$session_dir" wrist_left
        elif test -n "$right" && test -z "$left"; then
            assert_outputs "$session_dir" wrist_right
        else
            echo "one-device run must produce output for exactly one wrist" >&2
            exit 1
        fi
        ;;
    zero)
        assert_no_capture_files "$session_dir"
        ;;
esac

echo "PASS: mango wrist socket acceptance ($CASE)"
