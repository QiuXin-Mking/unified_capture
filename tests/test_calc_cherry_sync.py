import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from deploy.calc_cherry_sync import nearest_deltas


class CherrySyncTest(unittest.TestCase):
    def test_nearest_deltas_include_boundary_and_exclude_only_larger_values(self):
        self.assertEqual(nearest_deltas([0, 100_000, 100_001], [0]), [0, 100_000])

    def test_cli_flattens_deduplicates_and_reports_deterministic_statistics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session_001" / "cherry_stereo"
            root.mkdir(parents=True)
            self._write(root / "imu.jsonl", [
                {"gyro_samples": [{"pts_us": 100}, {"pts_us": 200}],
                 "acc_samples": [{"pts_us": 200}, {"pts_us": 300}]},
                {"gyro_samples": [{"pts_us": 400}], "acc_samples": []},
            ])
            self._write(root / "mag.jsonl", [
                {"samples": [{"pts_us": 100}, {"pts_us": 210}, {"pts_us": 400}]},
            ])
            self._write(root / "frame_meta.jsonl", [
                {"samples": [{"frame_pts_us": 100}, {"frame_pts_us": 300},
                             {"frame_pts_us": 400}]},
            ])

            result = self._run(Path(tmp))

            self.assertIn("IMU ↔ MAG", result)
            self.assertIn("配对 4 个", result)
            self.assertIn("≤10us: 3/4", result)
            self.assertIn("≤100us: 4/4", result)
            self.assertIn("Δus: p50=10  p90=90  min=0  max=90", result)
            self.assertIn("IMU ↔ FRAME_META", result)
            self.assertIn("MAG ↔ FRAME_META", result)

    def test_cli_reports_unavailable_comparisons_for_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "cherry_stereo"
            root.mkdir()
            self._write(root / "imu.jsonl", [{"gyro_samples": [{"pts_us": 1}], "acc_samples": []}])
            self._write(root / "mag.jsonl", [{"samples": [{"pts_us": 1}]}])

            result = self._run(Path(tmp))

            self.assertIn("IMU ↔ MAG", result)
            self.assertIn("IMU ↔ FRAME_META: unavailable", result)
            self.assertIn("MAG ↔ FRAME_META: unavailable", result)

    @staticmethod
    def _write(path, batches):
        path.write_text("".join(json.dumps(batch) + "\n" for batch in batches))

    @staticmethod
    def _run(session_dir):
        return subprocess.check_output(
            [sys.executable, "deploy/calc_cherry_sync.py", str(session_dir)], text=True)


if __name__ == "__main__":
    unittest.main()
