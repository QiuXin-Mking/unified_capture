#!/usr/bin/env python3
"""
calc_cherry_frame_imu_sync.py — Cherry 视频帧与 IMU 采样时间同步分析

通过 frame_meta.jsonl 中 frame_pts_us（MCU 时钟域）与 video_frames.jsonl
中 v4l2_timestamp_us（宿主机 CLOCK_MONOTONIC）的 frame_id → v4l2_sequence
对应关系，计算出两个时钟域的固定偏移，将全部 V4L2 帧时间戳转换到 MCU
时钟域后，与 imu.jsonl 的 pts_us 做最近邻匹配，输出同步统计。

用法: python3 deploy/calc_cherry_frame_imu_sync.py <session_dir>
      python3 deploy/calc_cherry_frame_imu_sync.py user@host:<session_path>
"""

import json
import statistics
import sys
import subprocess
import tempfile
from pathlib import Path
from typing import Optional


def nearest_deltas(a: list[int], b: list[int], max_delta_us: int = 100_000) -> list[int]:
    """返回每个 A 时间戳到最近 B 时间戳的差值（微秒），排除超过上限的。"""
    if not a or not b:
        return []
    deltas = []
    b_idx = 0
    for ts in a:
        while b_idx < len(b) and b[b_idx] < ts:
            b_idx += 1
        best = min((abs(b[i] - ts) for i in (b_idx - 1, b_idx)
                     if 0 <= i < len(b)), default=None)
        if best is not None and best <= max_delta_us:
            deltas.append(best)
    return deltas


def load_video_frames(path: Path) -> dict[int, int]:
    """从 video_frames.jsonl 加载 v4l2_sequence → v4l2_timestamp_us 映射。"""
    frames = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            frames[d["v4l2_sequence"]] = d["v4l2_timestamp_us"]
    return frames


def load_frame_meta(path: Path) -> dict[int, int]:
    """从 frame_meta.jsonl 加载 frame_id → frame_pts_us 映射（去重取最早）。"""
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            for s in json.loads(line).get("samples", []):
                fid = s["frame_id"]
                pts = s["frame_pts_us"]
                if fid not in meta:
                    meta[fid] = pts
    return meta


def load_imu_samples(path: Path) -> list[int]:
    """从 imu.jsonl 加载所有 IMU 采样的 pts_us（gyro + acc），去重排序。"""
    timestamps = set()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            for key in ("gyro_samples", "acc_samples"):
                for sample in d.get(key, []):
                    timestamps.add(sample["pts_us"])
    return sorted(timestamps)


def compute_clock_offset(video_frames: dict[int, int],
                         frame_meta: dict[int, int]) -> Optional[int]:
    """通过 frame_id ↔ v4l2_sequence 匹配计算 V4L2 → MCU 时钟偏移。

    offset = v4l2_timestamp_us - frame_pts_us
    返回偏移中位数，无匹配时返回 None。
    """
    offsets = []
    for frame_id, mcu_pts in frame_meta.items():
        v4l2_ts = video_frames.get(frame_id)
        if v4l2_ts is not None:
            offsets.append(v4l2_ts - mcu_pts)
    if not offsets:
        return None
    return int(statistics.median(offsets))


def print_sep(text: str):
    print(f"\n{'─'*60}")
    print(f"  {text}")
    print(f"{'─'*60}")


def print_stats(deltas: list[int]):
    """打印差值分布统计。"""
    sorted_d = sorted(deltas)
    n = len(sorted_d)
    p50 = sorted_d[n // 2]
    p90 = sorted_d[min(int(n * 0.9), n - 1)]
    within_10us = sum(1 for d in deltas if d <= 10)
    within_100us = sum(1 for d in deltas if d <= 100)

    print(f"      配对 {n} 个, "
          f"≤10us: {within_10us}/{n} ({within_10us/n*100:.1f}%)  "
          f"≤100us: {within_100us}/{n} ({within_100us/n*100:.1f}%)  "
          f"Δus: p50={p50}  p90={p90}  "
          f"min={sorted_d[0]}  max={sorted_d[-1]}")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else None
    if not target:
        print("用法: python3 deploy/calc_cherry_frame_imu_sync.py <session_dir>")
        print("      python3 deploy/calc_cherry_frame_imu_sync.py user@host:<session_path>")
        sys.exit(1)

    if ":" in target:
        tmp = tempfile.mkdtemp(prefix="cherry_fis_")
        subprocess.run(["rsync", "-avzq", target + "/", tmp + "/",
                        "--include=*/", "--include=*.jsonl", "--exclude=*"],
                       check=True)
        session_dir = tmp
    else:
        session_dir = target

    base = Path(session_dir)
    cherry_dirs = list(base.rglob("cherry_stereo"))
    if not cherry_dirs:
        subs = sorted(base.glob("session_*"))
        if subs:
            cherry_dirs = list(subs[0].rglob("cherry_stereo"))
    if not cherry_dirs:
        if (base / "video_frames.jsonl").exists():
            cherry_dirs = [base]
    if cherry_dirs:
        base = cherry_dirs[0]

    video_path = base / "video_frames.jsonl"
    imu_path = base / "imu.jsonl"
    meta_path = base / "frame_meta.jsonl"

    for p, name in [(video_path, "video_frames.jsonl"),
                    (imu_path, "imu.jsonl"),
                    (meta_path, "frame_meta.jsonl")]:
        if not p.exists():
            print(f"未找到 {name}: {base}")
            sys.exit(1)

    # ── 加载 ──
    video_frames = load_video_frames(video_path)
    frame_meta = load_frame_meta(meta_path)
    imu_ts = load_imu_samples(imu_path)

    if not video_frames:
        print("video_frames.jsonl 为空")
        sys.exit(1)
    if not imu_ts:
        print("imu.jsonl 无有效采样")
        sys.exit(1)

    # ── 计算时钟偏移 ──
    offset = compute_clock_offset(video_frames, frame_meta)
    if offset is None:
        print("frame_meta.jsonl 与 video_frames.jsonl 无匹配 frame_id，"
              "无法计算时钟偏移")
        sys.exit(1)

    matched_count = sum(1 for fid in frame_meta if fid in video_frames)
    offsets = [video_frames[fid] - frame_meta[fid]
               for fid in frame_meta if fid in video_frames]
    offset_spread = max(offsets) - min(offsets) if len(offsets) > 1 else 0

    # ── 转换 V4L2 帧时间到 MCU 时钟域 ──
    # 只转换与 IMU 时间范围有交集的帧（保留 video_frames 里的所有帧）
    video_ts_mcu = sorted(
        [ts - offset for ts in video_frames.values()]
    )

    # ── 基本信息 ──
    video_duration_s = (video_ts_mcu[-1] - video_ts_mcu[0]) / 1_000_000
    video_gaps = [video_ts_mcu[i+1] - video_ts_mcu[i]
                  for i in range(len(video_ts_mcu)-1)]
    video_med_gap = sorted(video_gaps)[len(video_gaps)//2] if video_gaps else 0

    imu_duration_s = (imu_ts[-1] - imu_ts[0]) / 1_000_000
    imu_gaps = [imu_ts[i+1] - imu_ts[i] for i in range(len(imu_ts)-1)]
    imu_med_gap = sorted(imu_gaps)[len(imu_gaps)//2] if imu_gaps else 0

    print_sep("时钟域标定")
    print(f"  frame_meta 条目: {len(frame_meta)} 个独立 frame_id")
    print(f"  frame_id ↔ v4l2_sequence 匹配: {matched_count} 个")
    print(f"  V4L2 → MCU 偏移中位数: {offset}us ({offset/1_000_000:.4f}s)")
    print(f"  偏移极差: {offset_spread}us "
          f"({'✓ 稳定' if offset_spread < 10_000 else '⚠ 不稳定'})")

    print_sep("各通道基本信息（统一在 MCU 时钟域）")
    video_avg_fps = len(video_ts_mcu) / video_duration_s if video_duration_s > 0 else 0
    print(f"  {'视频帧 (MCU 换算)':20s} {len(video_ts_mcu):5d}帧  "
          f"{video_duration_s:.1f}s  {video_avg_fps:.1f}fps  "
          f"帧间隔中位数: {video_med_gap}us")
    print(f"  {'IMU 采样 (MCU)':20s} {len(imu_ts):5d}个  "
          f"{imu_duration_s:.1f}s  "
          f"采样间隔中位数: {imu_med_gap}us")
    print(f"  {'采样/帧比':20s} {len(imu_ts)/len(video_ts_mcu):.1f}x  "
          f"(每帧约 {len(imu_ts)//len(video_ts_mcu)} 个 IMU 采样)")

    # ── 正向: 视频帧 → IMU ──
    print_sep("视频帧 ↔ IMU 采样 — 曝光终点差值分布")
    print(f"  （帧时间戳已通过 frame_meta 标定到 MCU 时钟域）")

    deltas_forward = nearest_deltas(video_ts_mcu, imu_ts)
    if not deltas_forward:
        print("  无匹配（时间无重叠）")
    else:
        print_stats(deltas_forward)

    # ── 反向: IMU → 视频帧 ──
    print_sep("IMU 采样 ↔ 视频帧 — 差值分布（反向）")
    deltas_reverse = nearest_deltas(imu_ts, video_ts_mcu)
    if not deltas_reverse:
        print("  无匹配")
    else:
        sorted_d = sorted(deltas_reverse)
        n = len(sorted_d)
        p50 = sorted_d[n // 2]
        p90 = sorted_d[min(int(n * 0.9), n - 1)]
        half_frame = video_med_gap // 2

        print(f"      配对 {n} 个,  "
              f"≤半帧({half_frame}us): "
              f"{sum(1 for d in deltas_reverse if d <= half_frame)}/{n} "
              f"({sum(1 for d in deltas_reverse if d <= half_frame)/n*100:.1f}%)  "
              f"Δus: p50={p50}  p90={p90}  "
              f"min={sorted_d[0]}  max={sorted_d[-1]}")

    print()


if __name__ == "__main__":
    main()
