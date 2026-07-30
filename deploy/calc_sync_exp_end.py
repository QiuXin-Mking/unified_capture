#!/usr/bin/env python3
"""
calc_sync_exp_end.py — 多相机曝光终点同步分析

从 IMU JSONL 提取每帧 exp_end_us, 按相机类型分组及跨组对比。
用法: python3 calc_sync_exp_end.py <session_dir>
      python3 calc_sync_exp_end.py user@host:<session_path>
"""

import json
import sys
import subprocess
import tempfile
from pathlib import Path


def load_exposures(path: str) -> list[int]:
    """返回去重+排序的 exp_end_us 列表"""
    starts = set()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            starts.add(d["exp_end_us"])
    return sorted(starts)


def print_sep(text: str):
    print(f"\n{'─'*60}")
    print(f"  {text}")
    print(f"{'─'*60}")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else None
    if not target:
        print("用法: python3 calc_sync_exp_end.py <session_dir>")
        print("      python3 calc_sync_exp_end.py user@host:<session_path>")
        sys.exit(1)

    if ":" in target:
        tmp = tempfile.mkdtemp(prefix="sync_")
        subprocess.run(["rsync", "-avzq", target + "/", tmp + "/",
                        "--include=*/", "--include=*.jsonl", "--exclude=*"],
                       check=True)
        session_dir = tmp
    else:
        session_dir = target

    subs = sorted(Path(session_dir).glob("session_*"))
    if subs:
        session_dir = str(subs[0])

    jsonl_files = sorted(Path(session_dir).rglob("*.jsonl"))
    jsonl_files = [f for f in jsonl_files
                   if "tracker" not in f.name and "encoder" not in f.name]

    if not jsonl_files:
        print(f"未找到 JSONL: {session_dir}")
        sys.exit(1)

    # ── 加载 ──
    cameras = {}
    for jf in jsonl_files:
        name = jf.parent.name
        starts = load_exposures(str(jf))
        if starts:
            cameras[name] = starts

    print_sep("各通道帧曝光统计")
    for name, starts in sorted(cameras.items()):
        duration_s = (starts[-1] - starts[0]) / 1_000_000
        avg_fps = len(starts) / duration_s if duration_s > 0 else 0
        gaps = [starts[i+1] - starts[i] for i in range(len(starts)-1)]
        med_gap_us = sorted(gaps)[len(gaps)//2] if gaps else 0
        print(f"  {name:16s} {len(starts):5d}帧  {duration_s:.1f}s  "
              f"{avg_fps:.1f}fps  exp_end: {starts[0]} → {starts[-1]}  "
              f"帧间隔中位数: {med_gap_us}us")

    # ── 分组: JHH (六目) vs wrist (腕部) — 不同时钟域, 分别对比 ──
    jhh = {k: v for k, v in cameras.items() if k.startswith("jhh")}
    wrist = {k: v for k, v in cameras.items() if "wrist" in k or k in ("SL", "JHHSW")}

    for group_name, group in [("六目相机 (JHH)", jhh), ("腕部相机 (Wrist)", wrist)]:
        if len(group) < 2:
            continue

        print_sep(f"{group_name} — 两两曝光终点差值分布")

        names = sorted(group.keys())
        for i, na in enumerate(names):
            for j, nb in enumerate(names):
                if i >= j:
                    continue
                sa = group[na]
                sb = group[nb]

                # 对每个 A 帧, 找最近的 B 帧, 记录差值
                deltas = []
                ib = 0
                for a_start in sa:
                    # 推进 sb 指针
                    while ib < len(sb) and sb[ib] < a_start:
                        ib += 1
                    # 检查 ib-1 和 ib 哪个更近
                    best = float('inf')
                    for k in [ib-1, ib]:
                        if 0 <= k < len(sb):
                            d = abs(sb[k] - a_start)
                            if d < best:
                                best = d
                    if best < 100_000:  # 忽略超过 100ms 的 (跨帧周期)
                        deltas.append(int(best))

                if not deltas:
                    print(f"  {na} ↔ {nb}: 无匹配 (时间无重叠)")
                    continue

                sorted_d = sorted(deltas)
                p50 = sorted_d[len(sorted_d)//2]
                p90 = sorted_d[int(len(sorted_d)*0.9)]
                within_10us = sum(1 for d in deltas if d <= 10)
                within_100us = sum(1 for d in deltas if d <= 100)

                print(f"  {na:16s} ↔ {nb:16s}  "
                      f"配对 {len(deltas)} 个, "
                      f"≤10us: {within_10us}/{len(deltas)} ({within_10us/len(deltas)*100:.1f}%)  "
                      f"≤100us: {within_100us}/{len(deltas)} ({within_100us/len(deltas)*100:.1f}%)  "
                      f"Δus: p50={p50}  p90={p90}  "
                      f"min={sorted_d[0]}  max={sorted_d[-1]}")

    # ── 跨组对比: JHH ↔ 腕部 ──
    cross_pairs = []
    if "jhh02" in cameras and "wrist_left" in cameras:
        cross_pairs.append(("jhh02", "wrist_left"))
    if "jhh04" in cameras and "wrist_left" in cameras:
        cross_pairs.append(("jhh04", "wrist_left"))

    if cross_pairs:
        print_sep("六目 (JHH) ↔ 腕部 (Wrist) — 曝光终点差值分布")

        for na, nb in cross_pairs:
            sa = cameras[na]
            sb = cameras[nb]

            # 对每个 A 帧, 找最近的 B 帧, 记录差值
            deltas = []
            ib = 0
            for a_start in sa:
                while ib < len(sb) and sb[ib] < a_start:
                    ib += 1
                best = float('inf')
                for k in [ib-1, ib]:
                    if 0 <= k < len(sb):
                        d = abs(sb[k] - a_start)
                        if d < best:
                            best = d
                if best < 100_000:
                    deltas.append(int(best))

            if not deltas:
                print(f"  {na} ↔ {nb}: 无匹配 (时间无重叠)")
                continue

            sorted_d = sorted(deltas)
            p50 = sorted_d[len(sorted_d)//2]
            p90 = sorted_d[int(len(sorted_d)*0.9)]
            within_10us = sum(1 for d in deltas if d <= 10)
            within_100us = sum(1 for d in deltas if d <= 100)

            print(f"  {na:16s} ↔ {nb:16s}  "
                  f"配对 {len(deltas)} 个, "
                  f"≤10us: {within_10us}/{len(deltas)} ({within_10us/len(deltas)*100:.1f}%)  "
                  f"≤100us: {within_100us}/{len(deltas)} ({within_100us/len(deltas)*100:.1f}%)  "
                  f"Δus: p50={p50}  p90={p90}  "
                  f"min={sorted_d[0]}  max={sorted_d[-1]}")

    print()


if __name__ == "__main__":
    main()
