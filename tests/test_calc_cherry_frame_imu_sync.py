import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from deploy.calc_cherry_frame_imu_sync import (
    compute_clock_offset,
    load_frame_meta,
    load_video_frames,
    nearest_deltas,
)


class CherryFrameImuSyncTest(unittest.TestCase):
    def test_nearest_deltas_include_boundary_and_exclude_only_larger_values(self):
        self.assertEqual(nearest_deltas([0, 100_000, 100_001], [0]), [0, 100_000])

    def test_nearest_deltas_empty_input(self):
        self.assertEqual(nearest_deltas([], [1, 2, 3]), [])
        self.assertEqual(nearest_deltas([1, 2, 3], []), [])

    def test_nearest_deltas_picks_closest_from_dense_b(self):
        imu_samples = list(range(0, 100_000, 1000))
        video_frames = [1500, 31500, 78000]
        deltas = nearest_deltas(video_frames, imu_samples)
        self.assertEqual(len(deltas), 3)
        self.assertEqual(deltas[0], 500)
        self.assertEqual(deltas[1], 500)
        self.assertEqual(deltas[2], 0)

    def test_compute_clock_offset_returns_median(self):
        video = {0: 100_000_000, 1: 100_033_333, 2: 100_066_666}
        meta = {0: 50_000_000, 1: 50_033_335, 2: 50_066_664}
        offset = compute_clock_offset(video, meta)
        # offsets: 50000000, 49999998, 50000002 → median=50000000
        self.assertEqual(offset, 50_000_000)

    def test_compute_clock_offset_no_match_returns_none(self):
        video = {0: 100}
        meta = {99: 50}
        self.assertIsNone(compute_clock_offset(video, meta))

    def test_compute_clock_offset_partial_match(self):
        video = {0: 100, 1: 200}
        meta = {0: 50, 99: 999}  # frame_id=99 not in video
        offset = compute_clock_offset(video, meta)
        self.assertEqual(offset, 50)  # only (0, 100-50) used

    def test_cli_full_pipeline_with_clock_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session_001" / "cherry_stereo"
            root.mkdir(parents=True)

            # V4L2 在宿主机时钟域，frame_meta 在 MCU 时钟域，差 ~56s
            OFFSET = 56_000_000
            num_frames = 100

            video = []
            for i in range(num_frames):
                video.append({"v4l2_sequence": i,
                              "v4l2_timestamp_us": OFFSET + 100_000_000 + i * 33_333})
            self._write(root / "video_frames.jsonl", video)

            # frame_meta 只覆盖前 10 帧
            meta_batches = []
            for i in range(10):
                meta_batches.append({
                    "generation": 1,
                    "samples": [
                        {"sensor_idx": 0, "vi_pipe": 0,
                         "frame_id": i,
                         "frame_pts_us": 100_000_000 + i * 33_333},
                        {"sensor_idx": 1, "vi_pipe": 1,
                         "frame_id": i,
                         "frame_pts_us": 100_000_000 + i * 33_333},
                    ]
                })
            self._write(root / "frame_meta.jsonl", meta_batches)

            # IMU 在 MCU 时钟域
            imu_batches = []
            sample_idx = 0
            for batch_start in range(100_000_000 - 5000,
                                      100_000_000 + num_frames * 33_333 + 5000,
                                      5000):
                gyro = []
                acc = []
                for _ in range(10):
                    ts = batch_start + sample_idx * 500
                    gyro.append({"x": 1, "y": 2, "z": 3,
                                 "temperature": 25, "pts_us": ts})
                    acc.append({"x": 1, "y": 2, "z": 3,
                                "temperature": 25, "pts_us": ts})
                    sample_idx += 1
                imu_batches.append({
                    "generation": batch_start // 5000,
                    "window_begin_pts_us": batch_start,
                    "window_end_pts_us": batch_start + 5000,
                    "gyro_samples": gyro,
                    "acc_samples": acc,
                })
            self._write(root / "imu.jsonl", imu_batches)

            result = self._run(Path(tmp))

            self.assertIn("时钟域标定", result)
            self.assertIn(str(OFFSET), result)
            self.assertIn("匹配: 10 个", result)
            self.assertIn("视频帧 ↔ IMU 采样", result)
            self.assertIn("配对 100 个", result)
            self.assertIn("Δus: p50=", result)
            self.assertIn("min=", result)
            self.assertIn("max=", result)

    def test_cli_handles_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "cherry_stereo"
            root.mkdir()

            self._write(root / "imu.jsonl", [
                {"generation": 0,
                 "window_begin_pts_us": 0, "window_end_pts_us": 1000,
                 "gyro_samples": [{"x": 1, "y": 2, "z": 3,
                                   "temperature": 25, "pts_us": 500}],
                 "acc_samples": []},
            ])

            proc = subprocess.run(
                [sys.executable, "deploy/calc_cherry_frame_imu_sync.py",
                 str(Path(tmp))],
                capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("video_frames.jsonl", proc.stdout + proc.stderr)

    def test_cli_finds_cherry_stereo_in_session_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session_001" / "cherry_stereo"
            root.mkdir(parents=True)

            self._write(root / "video_frames.jsonl", [
                {"v4l2_sequence": 0, "v4l2_timestamp_us": 100_000_000},
                {"v4l2_sequence": 1, "v4l2_timestamp_us": 100_033_333},
            ])
            self._write(root / "frame_meta.jsonl", [
                {"generation": 1,
                 "samples": [{"sensor_idx": 0, "vi_pipe": 0,
                              "frame_id": 0, "frame_pts_us": 100_000_000}]},
            ])
            self._write(root / "imu.jsonl", [
                {"generation": 0,
                 "window_begin_pts_us": 100_000_000,
                 "window_end_pts_us": 100_005_000,
                 "gyro_samples": [{"x": 1, "y": 2, "z": 3,
                                   "temperature": 25, "pts_us": 100_000_100}],
                 "acc_samples": []},
            ])

            result = self._run(Path(tmp))
            self.assertIn("视频帧 ↔ IMU 采样", result)

    def test_load_video_frames_returns_dict(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vf.jsonl"
            self._write(path, [
                {"v4l2_sequence": 5, "v4l2_timestamp_us": 500},
                {"v4l2_sequence": 3, "v4l2_timestamp_us": 300},
            ])
            frames = load_video_frames(path)
            self.assertEqual(frames, {5: 500, 3: 300})

    def test_load_frame_meta_deduplicates_by_frame_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fm.jsonl"
            self._write(path, [
                {"generation": 1,
                 "samples": [{"sensor_idx": 0, "vi_pipe": 0,
                              "frame_id": 1, "frame_pts_us": 100},
                             {"sensor_idx": 1, "vi_pipe": 1,
                              "frame_id": 1, "frame_pts_us": 100}]},
                {"generation": 2,
                 "samples": [{"sensor_idx": 0, "vi_pipe": 0,
                              "frame_id": 2, "frame_pts_us": 200}]},
            ])
            meta = load_frame_meta(path)
            self.assertEqual(meta, {1: 100, 2: 200})

    @staticmethod
    def _write(path, records):
        path.write_text("".join(json.dumps(r) + "\n" for r in records))

    @staticmethod
    def _run(session_dir):
        return subprocess.check_output(
            [sys.executable, "deploy/calc_cherry_frame_imu_sync.py",
             str(session_dir)], text=True)


if __name__ == "__main__":
    unittest.main()
