# Cherry Board End-to-End Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个 RK3588 板端一键验收脚本，完成 Cherry 真实数据采集、H.264 MKV/JSONL 完整性检查，并输出 IMU、MAG、GPIO FRAME_META 与视频帧结束时刻的 count/p50/p90/min/max 同步统计。

**Architecture:** `deploy/calc_cherry_sync.py` 负责加载四条时间线并返回可复用的结构化统计；`deploy/test_cherry_e2e.py` 负责板端预检、采集进程生命周期、产物验收与结果展示。真实采样使用唯一输出前缀且永不自动删除，同步数值不设通过阈值。

**Tech Stack:** Python 3 标准库（`argparse/json/pathlib/subprocess/signal/statistics`）、`unittest`、`ffprobe`、GNU Make、rsync/SSH。

## Global Constraints

- 视频帧结束时刻取 `video_frames.jsonl` 的 `v4l2_timestamp_us`，统计标识为 `VIDEO_END`。
- 输出 `IMU ↔ MAG`、`IMU ↔ VIDEO_END`、`MAG ↔ VIDEO_END`、`FRAME_META ↔ VIDEO_END` 四组最近邻绝对时间差。
- 每组只输出 `count/p50/p90/min/max`，单位为微秒；配对窗口为 `100_000 us`。
- 不对 p50/p90/min/max 设失败阈值；仅采集失败、产物不合法、时间线为空或无 100 ms 内配对时返回非零。
- 板端验收不修改 `/etc/unified_capture`，不启停 systemd，不删除采样数据。
- 保留现有 `deploy/calc_cherry_sync.py <session_dir|user@host:path>` CLI 能力。

---

### Task 1: VIDEO_END 时间线与结构化同步统计

**Files:**
- Modify: `deploy/calc_cherry_sync.py`
- Modify: `tests/test_calc_cherry_sync.py`

**Interfaces:**
- Produces: `summarize_deltas(deltas: list[int]) -> dict[str, int] | None`
- Produces: `build_sync_statistics(timelines: dict[str, list[int]]) -> list[tuple[str, str, dict[str, int] | None]]`
- Produces: `load_timelines(session_dir: Path) -> dict[str, list[int]]` with keys `IMU`, `MAG`, `FRAME_META`, `VIDEO_END`

- [ ] **Step 1: Write failing tests for VIDEO_END and exact statistics**

```python
from deploy.calc_cherry_sync import build_sync_statistics, load_timelines, summarize_deltas

def test_video_end_timeline_and_statistics(self):
    self._write(root / "video_frames.jsonl", [
        {"v4l2_timestamp_us": 100},
        {"v4l2_timestamp_us": 310},
    ])
    timelines = load_timelines(root)
    self.assertEqual(timelines["VIDEO_END"], [100, 310])
    self.assertEqual(summarize_deltas([90, 0, 10, 0]),
                     {"count": 4, "p50": 10, "p90": 90, "min": 0, "max": 90})
    labels = [(left, right) for left, right, _ in build_sync_statistics(timelines)]
    self.assertEqual(labels, [
        ("IMU", "MAG"),
        ("IMU", "VIDEO_END"),
        ("MAG", "VIDEO_END"),
        ("FRAME_META", "VIDEO_END"),
    ])
```

- [ ] **Step 2: Run the focused tests and verify RED**

Run: `python3 -m unittest tests.test_calc_cherry_sync.CherrySyncTest.test_video_end_timeline_and_statistics -v`

Expected: FAIL because `build_sync_statistics` and `summarize_deltas` do not exist.

- [ ] **Step 3: Implement the minimal reusable statistics API**

```python
COMPARISONS = (
    ("IMU", "MAG"),
    ("IMU", "VIDEO_END"),
    ("MAG", "VIDEO_END"),
    ("FRAME_META", "VIDEO_END"),
)

def summarize_deltas(deltas: list[int]) -> dict[str, int] | None:
    if not deltas:
        return None
    ordered = sorted(deltas)
    count = len(ordered)
    return {"count": count, "p50": ordered[count // 2],
            "p90": ordered[min(int(count * 0.9), count - 1)],
            "min": ordered[0], "max": ordered[-1]}

def build_sync_statistics(timelines):
    return [(left, right, summarize_deltas(nearest_deltas(timelines[left], timelines[right])))
            for left, right in COMPARISONS]
```

Load `VIDEO_END` by reading each nonblank `video_frames.jsonl` object directly and collecting `int(record["v4l2_timestamp_us"])`; update CLI output to exactly `LABEL: count=N p50=X p90=Y min=A max=B us`, or `LABEL: unavailable`.

- [ ] **Step 4: Run analyzer tests and verify GREEN**

Run: `python3 -m unittest tests/test_calc_cherry_sync.py -v`

Expected: all tests PASS and CLI contains all four labels with exact fields.

- [ ] **Step 5: Commit the analyzer extension**

```bash
git add deploy/calc_cherry_sync.py tests/test_calc_cherry_sync.py
git commit -m "feat: add Cherry video-end sync statistics"
```

### Task 2: Existing-output validation and analyse-only CLI

**Files:**
- Create: `deploy/test_cherry_e2e.py`
- Create: `tests/test_cherry_e2e.py`

**Interfaces:**
- Consumes: `build_sync_statistics()` and `load_timelines()` from Task 1
- Produces: `resolve_capture_dir(path: Path) -> Path`
- Produces: `validate_jsonl(capture_dir: Path) -> dict[str, int]`
- Produces: `probe_video(video_path: Path, runner=subprocess.run) -> dict[str, object]`
- Produces: `validate_capture(path: Path, runner=subprocess.run) -> dict[str, object]`

- [ ] **Step 1: Write failing unit tests for directory resolution and artifacts**

```python
def test_validate_capture_reports_video_json_and_four_stats(self):
    capture = self.make_capture(codec="h264", width=3200, height=1200,
                                fps="30/1", frame_count=12)
    report = validate_capture(capture.parent, runner=self.fake_ffprobe)
    self.assertEqual(report["capture_dir"], capture.resolve())
    self.assertEqual(report["video"]["codec"], "h264")
    self.assertEqual(report["jsonl_counts"], {
        "imu.jsonl": 1, "mag.jsonl": 1,
        "frame_meta.jsonl": 1, "video_frames.jsonl": 1,
    })
    self.assertEqual(len(report["sync"]), 4)

def test_validate_capture_rejects_bad_json(self):
    capture = self.make_capture()
    (capture / "mag.jsonl").write_text("not-json\n")
    with self.assertRaisesRegex(ValidationError, "mag.jsonl.*line 1"):
        validate_capture(capture, runner=self.fake_ffprobe)
```

Also cover missing/empty JSONL, missing/empty MKV, codec other than `h264`, resolution other than `3200x1200`, fps outside `29.0..31.0`, empty timeline, and a comparison with no 100 ms pair.

- [ ] **Step 2: Run the new test module and verify RED**

Run: `python3 -m unittest tests/test_cherry_e2e.py -v`

Expected: FAIL because `deploy.test_cherry_e2e` does not exist.

- [ ] **Step 3: Implement validation and analyse-only mode**

```python
class ValidationError(RuntimeError):
    pass

REQUIRED_JSONL = ("imu.jsonl", "mag.jsonl", "frame_meta.jsonl", "video_frames.jsonl")

def resolve_capture_dir(path: Path) -> Path:
    resolved = path.resolve()
    candidates = [resolved] if resolved.name == "cherry_stereo" else list(resolved.glob("cherry_stereo"))
    if not candidates:
        candidates = list(resolved.glob("session_*/cherry_stereo"))
    if len(candidates) != 1:
        raise ValidationError(f"expected exactly one cherry_stereo under {resolved}, found {len(candidates)}")
    return candidates[0]
```

Parse `ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=codec_name,width,height,avg_frame_rate,nb_read_frames,duration -of json`; validate H.264, 3200x1200, 29–31 fps, and nonempty video. Parse every JSONL line with filename and 1-based line numbers in errors. `main()` accepts `--analyse-only PATH`, prints artifact counts plus the four statistics, and exits `2` on `ValidationError`.

- [ ] **Step 4: Run validation tests and verify GREEN**

Run: `python3 -m unittest tests/test_cherry_e2e.py -v`

Expected: all existing-output success and failure cases PASS.

- [ ] **Step 5: Commit analyse-only validation**

```bash
git add deploy/test_cherry_e2e.py tests/test_cherry_e2e.py
git commit -m "feat: validate Cherry capture artifacts"
```

### Task 3: Timed real-capture process lifecycle

**Files:**
- Modify: `deploy/test_cherry_e2e.py`
- Modify: `tests/test_cherry_e2e.py`

**Interfaces:**
- Produces: `check_board_preconditions(binary: Path, product_config: Path) -> None`
- Produces: `run_capture(binary: Path, output_root: Path, duration_s: float, popen=subprocess.Popen, sleep=time.sleep) -> Path`
- Produces CLI: `test_cherry_e2e.py [--duration SEC] [--output-root PATH] [--binary PATH] [--analyse-only PATH]`

- [ ] **Step 1: Write failing lifecycle tests**

```python
def test_run_capture_uses_unique_prefix_and_sigint(self):
    fake = FakeProcess(returncode=0)
    capture_dir = run_capture(self.binary, self.output_root, 0,
                              popen=lambda *args, **kwargs: fake,
                              sleep=lambda _: None)
    self.assertEqual(fake.args[1:3], ["--no-gpio", "--single"])
    self.assertEqual(fake.signals, [signal.SIGINT])
    self.assertIn("cherry_e2e_", str(capture_dir))

def test_run_capture_kills_after_sigint_timeout(self):
    fake = FakeProcess(timeouts=True)
    with self.assertRaisesRegex(ValidationError, "did not stop after SIGINT"):
        run_capture(self.binary, self.output_root, 0,
                    popen=lambda *args, **kwargs: fake,
                    sleep=lambda _: None)
    self.assertTrue(fake.killed)
```

Also verify nonzero collector exit, no new session, multiple new sessions, missing binary, missing `ffprobe`, and active config without exact `product=cherry`.

- [ ] **Step 2: Run lifecycle tests and verify RED**

Run: `python3 -m unittest tests.test_cherry_e2e.CherryE2ETest.test_run_capture_uses_unique_prefix_and_sigint tests.test_cherry_e2e.CherryE2ETest.test_run_capture_kills_after_sigint_timeout -v`

Expected: FAIL because capture orchestration does not exist.

- [ ] **Step 3: Implement bounded signal handling and unique output discovery**

```python
command = [str(binary), "--no-gpio", "--single", str(output_prefix)]
process = popen(command, stdout=log_file, stderr=subprocess.STDOUT, text=True)
sleep(duration_s)
process.send_signal(signal.SIGINT)
try:
    return_code = process.wait(timeout=15)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait(timeout=5)
    raise ValidationError("capture did not stop after SIGINT; SIGKILL sent")
```

Use `<output-root>/cherry_e2e_YYYYmmdd_HHMMSS_<pid>` as the positional prefix. Snapshot its session directories before launch and require exactly one newly created `session_NNN/cherry_stereo` afterward. Tee output to `<prefix>.capture.log` while also printing it after teardown. Check `/etc/unified_capture/product.conf` has a non-comment `product=cherry`, binary is executable, `ffprobe` exists, and `/dev/video*` plus `/dev/ttyACM*` each have at least one node.

- [ ] **Step 4: Run all Python tests and verify GREEN**

Run: `python3 -m unittest tests/test_calc_cherry_sync.py tests/test_cherry_e2e.py -v`

Expected: all analyzer, validation and lifecycle tests PASS.

- [ ] **Step 5: Commit real-capture orchestration**

```bash
git add deploy/test_cherry_e2e.py tests/test_cherry_e2e.py
git commit -m "feat: add Cherry board end-to-end capture test"
```

### Task 4: Build integration and operator documentation

**Files:**
- Modify: `Makefile`
- Modify: `tests/test_source_layout.sh`
- Modify: `tests/README.md`
- Modify: `docs/design/cherry-profile-board-validation.md`

**Interfaces:**
- Consumes: CLI from Task 3
- Produces: `make test_cherry_e2e` and an operator command for a 30-second board run

- [ ] **Step 1: Add failing layout assertions**

```sh
test -f deploy/test_cherry_e2e.py
test -f tests/test_cherry_e2e.py
grep -q 'test_cherry_e2e' Makefile
python3 -m py_compile deploy/calc_cherry_sync.py deploy/test_cherry_e2e.py
```

Add `test_cherry_e2e` to the aggregate target check in `tests/test_source_layout.sh` before modifying `Makefile`.

- [ ] **Step 2: Run layout test and verify RED**

Run: `bash tests/test_source_layout.sh`

Expected: FAIL with `make test is missing test_cherry_e2e`.

- [ ] **Step 3: Wire the target and document exact operation**

```make
test_cherry_e2e:
	python3 -m unittest tests/test_cherry_e2e.py -v
```

Add it to `.PHONY` and `test:`. Add this board command to the validation document:

```bash
cd /root/unified_capture
python3 deploy/test_cherry_e2e.py --duration 30 --output-root /media/usb0/capture
```

Document that the script retains the absolute output path, `VIDEO_END` means `v4l2_timestamp_us`, and sync statistics have no pass/fail time threshold.

- [ ] **Step 4: Run the host regression suite**

Run: `make test && git diff --check`

Expected: all tests PASS and no whitespace errors.

- [ ] **Step 5: Commit integration and documentation**

```bash
git add Makefile tests/test_source_layout.sh tests/README.md docs/design/cherry-profile-board-validation.md
git commit -m "test: integrate Cherry board end-to-end validation"
```

### Task 5: RK3588 deployment and real-data acceptance

**Files:**
- Modify after collecting evidence: `docs/design/cherry-profile-board-validation.md`

**Interfaces:**
- Consumes: `deploy/sync_to_rk3588.sh` and `deploy/test_cherry_e2e.py`
- Produces: retained real capture directory and recorded four-group statistics

- [ ] **Step 1: Sync the verified tree to the board**

Run: `./deploy/sync_to_rk3588.sh`

Expected: rsync exits 0 and `/root/unified_capture/deploy/test_cherry_e2e.py` exists.

- [ ] **Step 2: Run board-side tests and rebuild**

Run: `ssh root@192.168.100.200 'cd /root/unified_capture && make test_cherry_e2e test_calc_cherry_sync && make -j2'`

Expected: both Python test suites PASS and `unified_capture` builds successfully.

- [ ] **Step 3: Perform a 30-second real capture**

Run: `ssh -o ServerAliveInterval=5 root@192.168.100.200 'cd /root/unified_capture && python3 deploy/test_cherry_e2e.py --duration 30 --output-root /media/usb0/capture'`

Expected: exit 0; MKV is H.264 3200x1200 at approximately 30 fps; all four JSONL counts are positive; all four comparisons print `count/p50/p90/min/max`; final output includes the retained absolute capture path.

- [ ] **Step 4: Record evidence without changing board configuration**

Append the exact capture path, video probe fields, JSONL counts and four statistic lines to `docs/design/cherry-profile-board-validation.md`. Confirm in the same evidence that `/etc/unified_capture/product.conf` remained `product=cherry` and systemd was not started or restarted.

- [ ] **Step 5: Run final verification and commit evidence**

Run: `make test && git diff --check && git status --short`

Expected: all tests PASS; only the evidence document is modified plus the pre-existing unrelated untracked paths.

```bash
git add docs/design/cherry-profile-board-validation.md
git commit -m "docs: record Cherry end-to-end sync validation"
```

