# Cherry Profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a production `cherry` profile for YCTC SC233HGS that records UVC H.264 into MKV, captures Sensor Bridge v3 IMU/MAG/FRAME_META JSONL, and analyzes synchronization.

**Architecture:** Device discovery pairs one H.264 V4L2 capture endpoint with one CDC ACM endpoint by their shared USB sysfs device parent. Two independent `Sensor` subclasses share a bounded start-state control so the serial START acknowledgement precedes V4L2 STREAMON; video is remuxed without MPP while serial frames are decoded into typed JSONL records.

**Tech Stack:** C++20, Linux V4L2, POSIX termios/poll, ffmpeg, Python 3, Make

## Global Constraints

- Preserve all existing mango and banana behavior and defaults.
- Use `hardware/cherry/`, not the misspelled `hardware/chery/` path from the original draft.
- Capture `V4L2_PIX_FMT_H264` at exactly `3200x1200@30`; do not decode MJPEG and do not invoke MPP for cherry.
- Remux H.264 with ffmpeg `-c copy` into `session_NNN/cherry_stereo/cherry_stereo.mkv`.
- Open CDC ACM at 921600 8N1, send START sequence 1 with mask `0x07`, and send STOP sequence 2 during teardown.
- Pair UVC and CDC endpoints by identical canonical USB device sysfs parent path plus VID/PID `0x5268:0x1218`; `busnum` is diagnostic only.
- Cherry never creates AS5600, VIVE, image-luma IMU, or Y8 producers.
- Keep the known FRAME_META hardware-wiring dependency observable; never invent missing samples.
- Use TDD for every behavior change and run each named test in both RED and GREEN phases.

---

### Task 0: Repair the pre-existing IMU luma test fixture

**Files:**
- Modify: `tests/test_imu_luma_decode.cpp`

**Interfaces:**
- No production interface changes.
- Restores the baseline `make test` contract after `IMU_TARGET` changed from 192 to 272 bytes in commit `313c5d6`.

- [ ] **Step 1: Preserve RED evidence**

  Run: `make test_imu_luma_decode`

  Expected: FAIL at `size == IMU_TARGET`; diagnostics established that both horizontal and vertical fixtures decode 192 bytes while `IMU_TARGET` is 272.

- [ ] **Step 2: Correct only the stale fixture**

  Change the fixture from 12 code bands to the protocol's current 17 bands, resize `decoded` from 256 to 384 bytes to match production `ImuFrame`, and assert all 17 decoded groups equal the literal payload. Do not modify `hardware/imu/imu_decode.h`.

- [ ] **Step 3: Verify GREEN and baseline**

  Run: `make test_imu_luma_decode && make test`

  Expected: both commands PASS with no assertions or warnings.

---

### Task 0b: Repair the pre-existing IMU frame-size assertion

**Files:**
- Modify: `tests/test_imu_frame_queue.cpp`

**Interfaces:**
- No production interface changes.
- Restores the queue test after commit `313c5d6` expanded `ImuFrame::data` from 256 to 384 bytes.

- [ ] **Step 1: Preserve RED evidence**

  Run: `make test_imu_frame_queue`

  Expected: compile failure because the stale `sizeof(ImuFrame) <= 288` assertion observes 408 bytes.

- [ ] **Step 2: Correct only the stale size budget**

  Update the test's size ceiling to the smallest aligned budget that accommodates the intentional 384-byte payload and existing metadata. Do not modify `hardware/imu/imu_frame_queue.h`.

- [ ] **Step 3: Verify GREEN and baseline**

  Run: `make test_imu_frame_queue && make test`

  Expected: both commands PASS, or `make test` advances to the next independently diagnosed pre-existing failure.

---

### Task 1: Sensor Bridge v3 protocol subset

**Files:**
- Create: `hardware/cherry/cherry_protocol.h`
- Create: `hardware/cherry/cherry_protocol.cpp`
- Create: `tests/test_cherry_protocol.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces `cherry::encode_start(uint16_t seq, uint8_t stream_mask) -> std::vector<uint8_t>`.
- Produces `cherry::encode_stop(uint16_t seq) -> std::vector<uint8_t>`.
- Produces `cherry::StreamParser::push(std::span<const uint8_t>) -> std::vector<Frame>`.
- Produces `decode_imu`, `decode_mag`, and `decode_frame_meta` returning `std::optional<TypedFrame>`.

- [ ] **Step 1: Write the failing protocol test**

  The test must assert these hand-derived frames and observable parser behavior:

  ```cpp
  assert(cherry::encode_start(1, 0x07) == std::vector<uint8_t>({
      0x53,0x59,0x03,0x01,0x00,0x01,0x00,0x04,
      0x00,0x00,0x07,0x00,0x00,0x00,0x6c,0x17}));
  assert(cherry::encode_stop(2) == std::vector<uint8_t>({
      0x53,0x59,0x03,0x02,0x00,0x02,
      0x00,0x00,0x00,0x00,0x0f,0x0f}));
  ```

  Feed the START frame one byte at a time and assert exactly one frame is emitted. Feed garbage + a CRC-corrupted frame + a valid STOP response and assert the parser resynchronizes, reports one decode error, and emits only the valid response. Build literal IMU/MAG/FRAME_META payloads containing negative signed values and 64-bit timestamps, then assert every decoded field.

- [ ] **Step 2: Verify RED**

  Run: `make test_cherry_protocol`

  Expected: FAIL because `hardware/cherry/cherry_protocol.h` and the Make target do not exist.

- [ ] **Step 3: Implement the minimal protocol**

  Define these frame contracts in `namespace cherry`:

  ```cpp
  constexpr uint16_t kMagic = 0x5953;
  constexpr uint8_t kVersion = 3;
  constexpr size_t kHeaderSize = 10;
  constexpr size_t kCrcSize = 2;
  constexpr size_t kMaxPayloadSize = 600;

  enum class MessageType : uint8_t {
      start = 0x01, stop = 0x02, imu_data = 0x04,
      mag_data = 0x05, frame_meta = 0x09, error = 0x0a,
  };

  struct Frame {
      MessageType type;
      uint8_t flags;
      uint16_t sequence;
      std::vector<uint8_t> payload;
  };
  ```

  Keep the parser buffer bounded to at most two maximum frames. On impossible payload length, invalid version/reserved/type contract, or CRC mismatch, discard one byte and scan for the next `53 59`. Copy payload bytes into each emitted `Frame`.

- [ ] **Step 4: Verify GREEN and mutation coverage**

  Run: `make test_cherry_protocol`

  Expected: PASS. Confirm that changing mask `0x07`, CRC polynomial `0xa001`, or signed little-endian decoding would fail at least one assertion.

---

### Task 2: Cherry product configuration

**Files:**
- Modify: `core/product_config.h`
- Modify: `core/product_config.cpp`
- Modify: `tests/test_product_config.cpp`
- Create: `tests/test_cherry_product_config.cpp`
- Modify: `deploy/product.conf.example`
- Modify: `deploy/camera-map.conf.example`
- Modify: `Makefile`

**Interfaces:**
- Extend `enum class ProductProfile` with `cherry`.
- Produce `CherryDeviceMap` with `vid`, `pid`, `width`, `height`, `fps`, `format`, `allow_missing_devices`, and optional wrist product names.
- Preserve `load_product_configuration(...) -> ProductConfigResult`.

- [ ] **Step 1: Write failing configuration tests**

  Create a temporary product file containing `product=cherry` and this literal camera map:

  ```ini
  [cherry]
  stereo.vid=0x5268
  stereo.pid=0x1218
  stereo.resolution=3200x1200
  stereo.format=H264
  stereo.fps=30
  allow_missing_devices=true
  ```

  Assert the parsed typed values. Add separate failure cases for malformed hex, `3200-1200`, format other than `H264`, fps other than 30, missing stereo keys, and duplicate entries. Extend the existing profile-name assertion with `product_profile_name(ProductProfile::cherry) == "cherry"`.

- [ ] **Step 2: Verify RED**

  Run: `make test_cherry_product_config`

  Expected: FAIL because `ProductProfile::cherry` and the target do not exist.

- [ ] **Step 3: Implement strict parsing and examples**

  Add:

  ```cpp
  struct CherryDeviceMap {
      uint16_t vid = 0x5268;
      uint16_t pid = 0x1218;
      int width = 3200;
      int height = 1200;
      int fps = 30;
      std::string format = "H264";
      bool allow_missing_devices = true;
      std::string wrist_left_product;
      std::string wrist_right_product;
  };
  ```

  Parse numeric values with full-string validation and bounds checks. Treat the wrist product keys as optional and omit empty placeholder assignments from the shipped example file.

- [ ] **Step 4: Verify GREEN and existing profiles**

  Run: `make test_cherry_product_config test_product_config`

  Expected: both PASS.

---

### Task 3: USB-parent device pairing

**Files:**
- Create: `hardware/cherry/cherry_discovery.h`
- Create: `hardware/cherry/cherry_discovery.cpp`
- Create: `tests/test_cherry_discovery.cpp`
- Modify: `hardware/video/device_discovery.h`
- Modify: `hardware/video/device_discovery.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produce `CherryVideoEndpoint {device_path, usb_parent, vid, pid, bus, supports_target}`.
- Produce `CherrySerialEndpoint {device_path, usb_parent, vid, pid, bus}`.
- Produce `match_cherry_device(const CherryDeviceMap&, videos, serials) -> CherryDiscoveryResult`.
- Extend `CameraDiscoveryResult` with `CherryDevices cherry`, where `cherry.stereo` is a `CameraSlot` and `cherry.serial_path` is the paired tty.

- [ ] **Step 1: Write the failing pure matcher test**

  Assert a video `/dev/video0` and tty `/dev/ttyACM0` with parent `/sys/.../2-1.1` pair even when another endpoint shares `bus=2`. Assert no pair for different parents, wrong VID/PID, missing H.264 target format, duplicate capture nodes, duplicate tty nodes, and metadata-only nodes marked `supports_target=false`.

- [ ] **Step 2: Verify RED**

  Run: `make test_cherry_discovery`

  Expected: FAIL because the discovery API does not exist.

- [ ] **Step 3: Implement matcher and sysfs inventory**

  Implement a helper that starts at the canonical class symlink target and walks parents until both `idVendor` and `idProduct` files exist. Scan tty names beginning with `ttyACM`. For each V4L2 capture node, enumerate `V4L2_PIX_FMT_H264`, discrete size `3200x1200`, and discrete interval `1/30`. Pass inventories into the pure matcher; do not match on `busnum` alone.

- [ ] **Step 4: Integrate dispatch and verify**

  In `discover_cameras`, dispatch `ProductProfile::cherry` to `discover_cherry_cameras`. Set `active_count=1` only when both endpoints are unambiguous. Run: `make test_cherry_discovery test_wrist_discovery`.

  Expected: both PASS.

---

### Task 4: Generic V4L2 format selection and H.264 frame writer

**Files:**
- Modify: `hardware/video/v4l2_device.h`
- Create: `hardware/cherry/cherry_h264_writer.h`
- Create: `hardware/cherry/cherry_h264_writer.cpp`
- Create: `tests/test_cherry_h264_writer.cpp`
- Modify: `Makefile`

**Interfaces:**
- Change `V4l2Device::open(path, width, height, fps, pixel_format = V4L2_PIX_FMT_MJPEG)` without changing existing callers.
- Produce `CherryH264Writer::process(const CompressedFrame&) -> VideoFrameProcessResult`, `finish()`, `bytes()`, and `error()`.

- [ ] **Step 1: Write the failing H.264 writer test**

  Use real temporary files and a literal `CompressedFrame` with bytes `{0x00,0x00,0x00,0x01,0x65,0xaa}`, sequence `42`, and timestamp `123456789`. Assert the H.264 file bytes are identical and the JSONL line parses to:

  ```json
  {"v4l2_sequence":42,"v4l2_timestamp_us":123456789}
  ```

  Construct a second writer with its video `FILE*` opened in read-only mode, call `process`, and assert `encoder_failure` with a non-empty error. This exercises a real write failure without using a closed/dangling `FILE*`.

- [ ] **Step 2: Verify RED**

  Run: `make test_cherry_h264_writer`

  Expected: FAIL because the writer does not exist.

- [ ] **Step 3: Implement format verification and writer**

  `V4l2Device::open` must request the supplied FourCC and reject a driver-returned FourCC, width, or height that differs. Log the selected FourCC by converting its four bytes to printable characters. The writer must use complete-write checks for both H.264 and JSONL and flush JSONL after each line so abrupt capture diagnostics remain available.

- [ ] **Step 4: Verify GREEN and V4L2 regressions**

  Run: `make test_cherry_h264_writer test_v4l2_frame_view test_capture_pipeline`

  Expected: all PASS.

---

### Task 5: Coordinated Cherry video and serial Sensors

**Files:**
- Create: `hardware/cherry/cherry_start_control.h`
- Create: `hardware/cherry/cherry_video_sensor.h`
- Create: `hardware/cherry/cherry_video_sensor.cpp`
- Create: `hardware/cherry/cherry_serial_sensor.h`
- Create: `hardware/cherry/cherry_serial_sensor.cpp`
- Create: `tests/test_cherry_start_control.cpp`
- Create: `tests/test_cherry_json.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produce `CherryStartControl::mark_ready()`, `mark_failed(std::string)`, and `wait(std::chrono::milliseconds) -> CherryStartResult`.
- `CherryVideoSensor` consumes `CameraConfig`, video path, session directory, running flag, and shared start control.
- `CherrySerialSensor` consumes tty path, sensor name, session directory, running flag, and shared start control.

- [ ] **Step 1: Write failing control and JSON tests**

  Assert a waiting thread wakes with ready, wakes with the exact failure text, and times out without deadlock. For each typed protocol frame, write JSON through the production serializer into a temporary file and assert generation, counts, signed sensor values, temperatures, raw magnetometer values, frame IDs, and 64-bit timestamps after parsing with Python or literal string-independent field checks.

- [ ] **Step 2: Verify RED**

  Run: `make test_cherry_start_control test_cherry_json`

  Expected: FAIL because the control and serializers do not exist.

- [ ] **Step 3: Implement `CherrySerialSensor`**

  Configure raw 8N1 with Linux `B921600`, `VMIN=0`, `VTIME=1`, no flow control. In setup, open all three output files, send START, poll for at most 2000 ms, process stream frames arriving before the ACK, and require response flag `0x01`, type START, sequence 1, and echoed mask `0x07`. Every setup failure calls `mark_failed`. Collect polls in 200 ms increments and continues after parser-level corrupted frames. Teardown sends STOP sequence 2 when the fd is valid and drains for at most 200 ms.

- [ ] **Step 4: Implement `CherryVideoSensor`**

  Wait at most 3000 ms for serial ready, open the video node as H.264, create a unique FIFO containing the process id and sensor name, spawn ffmpeg with `-f h264` to produce the exact `cherry_stereo.mkv`, and run `run_capture_pipeline` with `CherryH264Writer`. On teardown, stop V4L2, close the FIFO, wait for ffmpeg, remove the FIFO, and report acquired/processed/gap/overflow/byte statistics.

- [ ] **Step 5: Verify GREEN and compile host-testable units**

  Run: `make test_cherry_start_control test_cherry_json test_cherry_protocol test_cherry_h264_writer`

  Expected: all PASS.

---

### Task 6: Session, status, and runtime integration

**Files:**
- Modify: `app/session_profile.cpp`
- Modify: `app/session_runner.cpp`
- Modify: `app/runtime.cpp`
- Modify: `tests/test_session_profile.cpp`
- Modify: `tests/test_status_response.cpp`
- Modify: `tests/test_capture_output_policy.cpp`
- Modify: `Makefile`

**Interfaces:**
- `active_profile_cameras` returns only `cherry.stereo` for cherry.
- `profile_cameras_json` and runtime status expose `"cherry_stereo": true|false`.
- `SessionRunner` creates exactly two cherry Sensors sharing one `CherryStartControl`.

- [ ] **Step 1: Write failing profile and policy tests**

  Construct a cherry discovery result with an enabled stereo slot and serial path. Assert one active camera and exact camera JSON. Extend status tests to assert profile `cherry`, IMU enabled, AS5600 false, and VIVE false; cherry uses its dedicated H.264 Sensor and does not reuse the legacy H.265 `CameraOutputPolicy`.

- [ ] **Step 2: Verify RED**

  Run: `make test_session_profile test_capture_output_policy test_status_response`

  Expected: at least one new cherry assertion FAILS.

- [ ] **Step 3: Implement application branches**

  Add an explicit cherry branch before banana/mango fallbacks. `make_session_dir` creates only `cherry_stereo`. Runtime rejects the legacy `--no-h265` flag for cherry with an error explaining that H.264 video is mandatory, never calls `detect_vive_trackers`, sets AS5600 false, and requires the stereo pair. SessionRunner creates serial first in the vector and video second, but relies on `CherryStartControl`, not vector scheduling, for the START-before-STREAMON guarantee.

- [ ] **Step 4: Verify GREEN and all profile tests**

  Run: `make test_product_config test_cherry_product_config test_cherry_discovery test_session_profile test_capture_output_policy test_status_response`

  Expected: all PASS.

---

### Task 7: Synchronization analysis tool

**Files:**
- Create: `deploy/calc_cherry_sync.py`
- Create: `tests/test_calc_cherry_sync.py`
- Modify: `Makefile`

**Interfaces:**
- Produce `nearest_deltas(a: list[int], b: list[int], max_delta_us=100000) -> list[int]`.
- CLI accepts a local directory or `user@host:<path>` and prints three named comparisons when inputs exist.

- [ ] **Step 1: Write the failing Python test**

  Build a temporary `cherry_stereo` directory containing literal IMU, MAG, and FRAME_META JSONL batches. Assert nearest deltas for boundary values, strict exclusion of values greater than 100000 us, p50/p90/min/max, and exact counts for `<=10us` and `<=100us`. Assert a missing FRAME_META file still reports IMU-to-MAG and labels the other comparisons unavailable.

- [ ] **Step 2: Verify RED**

  Run: `python3 -m unittest tests/test_calc_cherry_sync.py -v`

  Expected: FAIL because the module does not exist.

- [ ] **Step 3: Implement local and remote loading**

  Parse every batch and flatten gyro + accelerometer sample `pts_us` into one deduplicated sorted IMU timeline; flatten MAG and FRAME_META similarly. For remote input, use a temporary directory context and `rsync` only `*.jsonl`. Use the same nearest-neighbor pointer method as `calc_sync_exp_end.py`, with deterministic nearest-rank indices `len//2` and `int(len*0.9)` capped to the final element.

- [ ] **Step 4: Verify GREEN**

  Run: `python3 -m unittest tests/test_calc_cherry_sync.py -v`

  Expected: PASS.

---

### Task 8: Build integration, documentation, and RK3588 acceptance

**Files:**
- Modify: `Makefile`
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `tests/README.md`
- Modify: `tests/test_source_layout.sh`
- Modify: `.trellis/tasks/08-03-cherry-profile/implement.md`
- Create: `.trellis/tasks/08-03-cherry-profile/research/2026-08-03-board-validation.md`

**Interfaces:**
- `make test` includes all new host-only tests.
- Production `CPP_SOURCES` includes every non-header Cherry unit.
- Deployment uses `deploy/sync_to_rk3588.sh` and the fixed target `/root/unified_capture`.

- [x] **Step 1: Add source-layout expectations and verify RED**

  Require the Cherry production files, tests, config examples, and analysis script in `tests/test_source_layout.sh`. Run: `make test_source_layout`.

  Expected: FAIL until the full file list and build wiring are present.

- [x] **Step 2: Complete Make and documentation wiring**

  Add Cherry `.cpp` files to `CPP_SOURCES`, add phony test targets and dependencies, and add them to `test`. Update architecture/profile/config/output documentation with H.264 pass-through, USB-parent pairing, serial streams, and the FRAME_META wiring caveat. Do not describe wrist hardware as available.

- [x] **Step 3: Run fresh local verification**

  Run:

  ```bash
  make clean
  make test
  bash tests/test_sync_to_rk3588.sh
  python3 -m unittest tests/test_calc_cherry_sync.py -v
  git diff --check
  ```

  Expected: all commands exit zero. If the known `test_imu_luma_decode` host fixture still fails, diagnose it under systematic-debugging and either fix the fixture without changing production behavior or document evidence that it is an environment-specific pre-existing failure before board validation.

- [x] **Step 4: Sync and build on RK3588**

  Run:

  ```bash
  ./deploy/sync_to_rk3588.sh
  ssh root@192.168.100.200 'cd /root/unified_capture && make clean && make test && make'
  ```

  Expected: host tests and production link exit zero on aarch64.

- [x] **Step 5: Back up configuration and scan**

  On the board, create timestamped copies of `/etc/unified_capture/product.conf` and `camera-map.conf`, install the confirmed cherry sections, then run `/root/unified_capture/unified_capture --scan`. Expected: one `cherry_stereo` pair showing `/dev/video0` and `/dev/ttyACM0` with USB parent `2-1.1`.

- [x] **Step 6: Record a bounded hardware session**

  Run the binary in no-GPIO single-session mode under `timeout --signal=INT 30s`, writing beneath `/media/usb0/capture/`. Do not modify or restart the systemd service. Expected: controlled teardown sends STOP and ffmpeg exits normally.

- [x] **Step 7: Validate artifacts and synchronization**

  Use `ffprobe` to assert H.264 codec, 3200x1200 dimensions, and approximately 30 fps. Parse every JSONL line with Python, record line counts and first/last samples, then run `deploy/calc_cherry_sync.py`. Mark FRAME_META validation as hardware-blocked if its file is valid but empty; IMU, MAG, and MKV remain required.

- [x] **Step 8: Persist evidence and update checkboxes**

  Write exact commands, exit codes, artifact sizes/counts, ffprobe fields, sync statistics, and any wiring dependency to `research/2026-08-03-board-validation.md`. Check completed steps in this file only after fresh evidence exists.
