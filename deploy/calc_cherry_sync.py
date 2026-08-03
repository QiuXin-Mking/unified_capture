#!/usr/bin/env python3
"""Report timestamp synchronization for Cherry IMU, MAG and frame metadata."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def nearest_deltas(a: list[int], b: list[int], max_delta_us: int = 100_000) -> list[int]:
    """Return each A timestamp's nearest B delta, excluding deltas over the limit."""
    if not a or not b:
        return []
    deltas = []
    b_index = 0
    for timestamp in a:
        while b_index < len(b) and b[b_index] < timestamp:
            b_index += 1
        best = min((abs(b[index] - timestamp) for index in (b_index - 1, b_index)
                    if 0 <= index < len(b)), default=None)
        if best is not None and best <= max_delta_us:
            deltas.append(best)
    return deltas


def _load_timeline(path: Path, sample_key: str, timestamp_key: str) -> list[int]:
    timestamps = set()
    for line in path.read_text().splitlines():
        if line.strip():
            for sample in json.loads(line).get(sample_key, []):
                if timestamp_key in sample:
                    timestamps.add(int(sample[timestamp_key]))
    return sorted(timestamps)


def load_timelines(session_dir: Path) -> dict[str, list[int]]:
    """Load every Cherry JSONL batch into sorted, deduplicated timelines."""
    files = list(session_dir.rglob("*.jsonl"))
    imu = set()
    for path in (item for item in files if item.name == "imu.jsonl"):
        for key in ("gyro_samples", "acc_samples"):
            imu.update(_load_timeline(path, key, "pts_us"))

    def load_named(filename: str, timestamp_key: str) -> list[int]:
        values = set()
        for path in (item for item in files if item.name == filename):
            values.update(_load_timeline(path, "samples", timestamp_key))
        return sorted(values)

    return {"IMU": sorted(imu), "MAG": load_named("mag.jsonl", "pts_us"),
            "FRAME_META": load_named("frame_meta.jsonl", "frame_pts_us")}


def _print_comparison(left_name: str, left: list[int], right_name: str, right: list[int]) -> None:
    label = f"{left_name} ↔ {right_name}"
    if not left or not right:
        print(f"{label}: unavailable")
        return
    deltas = nearest_deltas(left, right)
    if not deltas:
        print(f"{label}: 无匹配 (时间无重叠)")
        return
    ordered = sorted(deltas)
    count = len(ordered)
    p50 = ordered[count // 2]
    p90 = ordered[min(int(count * 0.9), count - 1)]
    within_10 = sum(delta <= 10 for delta in ordered)
    within_100 = sum(delta <= 100 for delta in ordered)
    print(f"{label}: 配对 {count} 个, "
          f"≤10us: {within_10}/{count} ({within_10 / count * 100:.1f}%)  "
          f"≤100us: {within_100}/{count} ({within_100 / count * 100:.1f}%)  "
          f"Δus: p50={p50}  p90={p90}  min={ordered[0]}  max={ordered[-1]}")


def analyse(session_dir: Path) -> None:
    timelines = load_timelines(session_dir)
    _print_comparison("IMU", timelines["IMU"], "MAG", timelines["MAG"])
    _print_comparison("IMU", timelines["IMU"], "FRAME_META", timelines["FRAME_META"])
    _print_comparison("MAG", timelines["MAG"], "FRAME_META", timelines["FRAME_META"])


def main() -> None:
    if len(sys.argv) != 2:
        print("用法: python3 deploy/calc_cherry_sync.py <session_dir|user@host:path>")
        raise SystemExit(1)
    target = sys.argv[1]
    if ":" not in target:
        analyse(Path(target))
        return
    with tempfile.TemporaryDirectory(prefix="cherry_sync_") as tmp:
        subprocess.run(["rsync", "-avzq", target + "/", tmp + "/", "--include=*/",
                        "--include=*.jsonl", "--exclude=*"], check=True)
        analyse(Path(tmp))


if __name__ == "__main__":
    main()
