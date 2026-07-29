# Native V4L2 Capture Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all production Nori SDK discovery and streaming with native V4L2 while preserving H.265, Y8, IMU, preview, session behavior, and four-camera 30 fps capture.

**Architecture:** Discover UVC capture nodes through V4L2 plus sysfs identity, store stable node metadata in camera configuration, and stream with a reusable MMAP wrapper. Acquisition threads copy compressed MJPEG and immediately requeue driver buffers; ordered processing threads perform decode, IMU extraction, NV12 conversion, MPP encoding, and file output.

**Tech Stack:** C++20, Linux V4L2/uvcvideo, sysfs, `poll`, `mmap`, TurboJPEG, Rockchip MPP, FFmpeg, existing host test harness.

## Global Constraints

- Do not use `Nori_Xvision_API.h` or link `libNori_Xvision_Std.so` in production.
- Do not hardcode `/dev/videoN`; match VID, PID, product, serial, bus, and USB path.
- Preserve banana and mango configuration syntax and output directory layout.
- Preserve H.265, Y8, IMU, preview, socket, GPIO, and session behavior.
- Preserve JHH02 → JHH04 → wrist stream-start ordering.
- Copy compressed MJPEG and requeue the V4L2 driver buffer before decoding or writing.
- Processing must remain ordered per camera.
- Queue overflow and V4L2 sequence gaps must be counted and treated as validation failures.
- Board acceptance is four cameras for 30 seconds, approximately 900 frames each, with all configured outputs valid.

---

### Task 1: Restore a Green Host Baseline

**Files:**
- Modify: `tests/test_wrist_discovery.cpp`

**Interfaces:**
- Consumes: `make_wrist_left_config` and `make_wrist_right_config`.
- Produces: a host suite whose wrist orientation expectation matches production `HORIZONTAL_TOP`.

- [ ] **Step 1: Verify the existing failure**

Run:

```bash
make test_wrist_discovery
```

Expected: FAIL at `assert_wrist_encoding` because the test expects
`VERTICAL_LEFT` while production returns `HORIZONTAL_TOP`.

- [ ] **Step 2: Correct the stale assertion**

Change:

```cpp
assert(camera.config.imu_orientation == ImuOrientation::VERTICAL_LEFT);
```

to:

```cpp
assert(camera.config.imu_orientation == ImuOrientation::HORIZONTAL_TOP);
```

- [ ] **Step 3: Verify the complete baseline**

Run:

```bash
make test
```

Expected: exit `0`.

- [ ] **Step 4: Commit**

```bash
git add tests/test_wrist_discovery.cpp
git commit -m "test: align wrist IMU orientation expectation"
```

---

### Task 2: Add Stable V4L2 Device Inventory and Matching

**Files:**
- Create: `hardware/video/v4l2_discovery.h`
- Create: `hardware/video/v4l2_discovery.cpp`
- Create: `tests/test_v4l2_discovery.cpp`
- Modify: `core/camera_config.h`
- Modify: `hardware/wrist/wrist_discovery.h`
- Modify: `hardware/wrist/wrist_discovery.cpp`
- Modify: `hardware/wrist/wrist_profile.cpp`
- Modify: `hardware/video/device_discovery.h`
- Modify: `hardware/video/device_discovery.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct V4l2Format {
    bool is_mjpeg;
    int width;
    int height;
    int fps;
};

struct V4l2DeviceInfo {
    std::string node;
    uint16_t vid;
    uint16_t pid;
    std::string product;
    std::string serial;
    std::string usb_path;
    int bus_number;
    std::vector<V4l2Format> formats;
};

std::vector<V4l2DeviceInfo> enumerate_v4l2_devices();
CameraDiscoveryResult match_v4l2_cameras(
    const ProductConfiguration& configuration,
    const std::vector<V4l2DeviceInfo>& inventory);
```

- Changes `CameraConfig` to carry `std::string device_path`,
  `std::string serial`, `std::string usb_path`, and `int bus_number`.
- Changes `SixCamDevices` to carry full `CameraConfig jhh04` and
  `CameraConfig jhh02` slots instead of Nori numeric IDs.

- [ ] **Step 1: Write the failing pure matching test**

Create `tests/test_v4l2_discovery.cpp` with inventory entries whose node numbers
are deliberately out of order:

```cpp
#include "hardware/video/v4l2_discovery.h"

#include <cassert>

int main() {
    ProductConfiguration configuration;
    configuration.profile = ProductProfile::banana;
    configuration.wrist.allow_missing_devices = false;
    configuration.wrist.left_product = "SL";
    configuration.wrist.right_product = "JHHSW";
    configuration.sixcam_enabled = true;

    const V4l2Format wrist{true, 1440, 960, 30};
    const V4l2Format jhh02{true, 4000, 1200, 30};
    const V4l2Format jhh04{true, 3104, 480, 30};
    const std::vector<V4l2DeviceInfo> inventory{
        {"/dev/video6", 0x1bcf, 0x2d52, "JHHSW", "SW002", "2-1.2.4", 2, {wrist}},
        {"/dev/video2", 0x1bcf, 0x2d51, "JHH", "SIX001", "6-1.2", 6, {jhh04}},
        {"/dev/video4", 0x1bcf, 0x2d52, "SL", "SL003", "2-1.2.3", 2, {wrist}},
        {"/dev/video0", 0x1bcf, 0x2d50, "JHH", "SIX001", "6-1.1", 6, {jhh02}},
    };

    const CameraDiscoveryResult result =
        match_v4l2_cameras(configuration, inventory);
    assert(result.active_count == 4);
    assert(result.wrist[0].config.device_path == "/dev/video4");
    assert(result.wrist[1].config.device_path == "/dev/video6");
    assert(result.sixcam.jhh02.device_path == "/dev/video0");
    assert(result.sixcam.jhh04.device_path == "/dev/video2");
    assert(result.sixcam.jhh02.bus_number == result.sixcam.jhh04.bus_number);
    return 0;
}
```

- [ ] **Step 2: Add the target and verify RED**

Add `test_v4l2_discovery` to `test` and `.PHONY`, compiling
`tests/test_v4l2_discovery.cpp`, `hardware/video/v4l2_discovery.cpp`,
`hardware/wrist/wrist_discovery.cpp`, and `hardware/wrist/wrist_profile.cpp`.

Run:

```bash
make test_v4l2_discovery
```

Expected: compilation fails because `v4l2_discovery.h` does not exist.

- [ ] **Step 3: Add stable identity fields**

Replace `CameraConfig::device_id` with:

```cpp
std::string device_path;
std::string serial;
std::string usb_path;
int bus_number = -1;
```

Update aggregate initializers so unmatched cameras contain empty strings and
`bus_number == -1`.

Replace `WristDeviceInfo::device_id` with the same four identity fields and
copy them into the matched `CameraConfig`.

- [ ] **Step 4: Implement pure inventory matching**

Implement `match_v4l2_cameras` using the existing wrist mapping rules. For the
six-camera pair:

```cpp
// Select one 1bcf:2d51 JHH04.
// Select one 1bcf:2d50 JHH02 whose bus_number matches JHH04.
// Reject missing or ambiguous candidates with camera_errors.
```

Require exact configured MJPEG width, height, and fps before enabling a slot.
Do not compare node numbers.

- [ ] **Step 5: Verify GREEN and add edge cases**

Extend the test to assert:

- metadata/non-capture nodes are absent from the supplied inventory;
- duplicate `SL` products disable wrist-left;
- JHH02 on a different bus does not pair with JHH04;
- missing MJPEG 30 fps disables the affected camera;
- `allow_missing_devices=true` produces degraded banana discovery.

Run:

```bash
make test_v4l2_discovery
make test_wrist_discovery
```

Expected: both exit `0`.

- [ ] **Step 6: Implement Linux enumeration**

In `enumerate_v4l2_devices`:

1. iterate `/sys/class/video4linux/video*`;
2. open `/dev/<entry>`;
3. call `VIDIOC_QUERYCAP`;
4. require `V4L2_CAP_VIDEO_CAPTURE` and `V4L2_CAP_STREAMING`;
5. walk the resolved sysfs parent path until `idVendor`, `idProduct`,
   `product`, and `serial` are found;
6. record the USB basename such as `2-1.2.3` and parse the bus prefix;
7. enumerate MJPEG formats, frame sizes, and frame intervals with
   `VIDIOC_ENUM_FMT`, `VIDIOC_ENUM_FRAMESIZES`, and
   `VIDIOC_ENUM_FRAMEINTERVALS`.

Return errors as skipped-node diagnostics; never invent identity values.

- [ ] **Step 7: Replace Nori discovery**

Make `discover_cameras` call:

```cpp
return match_v4l2_cameras(configuration, enumerate_v4l2_devices());
```

Make `scan_devices` print node, VID/PID, product, serial, USB path, and formats.
Remove `Nori_Xvision_API.h` from discovery.

- [ ] **Step 8: Verify and commit**

Run:

```bash
make test
git diff --check
```

Expected: exit `0`.

Commit:

```bash
git add core/camera_config.h hardware/video/v4l2_discovery.* \
  hardware/video/device_discovery.* hardware/wrist tests/test_v4l2_discovery.cpp \
  Makefile
git commit -m "feat: discover cameras through V4L2 and sysfs"
```

---

### Task 3: Add the Ordered Compressed-Frame Queue

**Files:**
- Create: `hardware/video/compressed_frame_queue.h`
- Create: `tests/test_compressed_frame_queue.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct CompressedFrame {
    uint32_t sequence;
    uint64_t timestamp_us;
    std::vector<uint8_t> mjpeg;
};

class CompressedFrameQueue {
public:
    explicit CompressedFrameQueue(std::size_t capacity);
    bool try_push(CompressedFrame frame);
    bool pop_wait(CompressedFrame* frame);
    void close();
    std::size_t high_water_mark() const;
    uint64_t overflow_count() const;
};
```

- `pop_wait` returns `false` only when the queue is closed and empty.
- `try_push` returns `false` and increments overflow when full or closed.

- [ ] **Step 1: Write the failing queue test**

Create `tests/test_compressed_frame_queue.cpp`:

```cpp
#include "hardware/video/compressed_frame_queue.h"

#include <cassert>

int main() {
    CompressedFrameQueue queue(2);
    assert(queue.try_push({10, 1000, {1}}));
    assert(queue.try_push({11, 2000, {2}}));
    assert(!queue.try_push({12, 3000, {3}}));
    assert(queue.high_water_mark() == 2);
    assert(queue.overflow_count() == 1);

    CompressedFrame frame;
    assert(queue.pop_wait(&frame) && frame.sequence == 10);
    assert(queue.pop_wait(&frame) && frame.sequence == 11);
    queue.close();
    assert(!queue.pop_wait(&frame));
    return 0;
}
```

- [ ] **Step 2: Add target and verify RED**

Add `test_compressed_frame_queue` to `test` and `.PHONY`.

Run `make test_compressed_frame_queue`.

Expected: compilation fails because the header does not exist.

- [ ] **Step 3: Implement the bounded queue**

Use `std::deque`, `std::mutex`, and `std::condition_variable`. Update
high-water mark while holding the mutex. `close()` must notify all waiters.
Delete copy construction and copy assignment.

- [ ] **Step 4: Add shutdown and ordering coverage**

Add a consumer thread blocked in `pop_wait`, call `close`, join it, and assert
it returned `false`. Add 100 sequential frames and verify sequence order.

- [ ] **Step 5: Verify and commit**

Run:

```bash
make test_compressed_frame_queue
make test
git diff --check
```

Commit:

```bash
git add hardware/video/compressed_frame_queue.h \
  tests/test_compressed_frame_queue.cpp Makefile
git commit -m "feat: add ordered compressed frame queue"
```

---

### Task 4: Add a Reusable V4L2 MMAP Capture Device

**Files:**
- Create: `hardware/video/v4l2_capture_device.h`
- Create: `hardware/video/v4l2_capture_device.cpp`
- Create: `hardware/video/v4l2_capture_ops.h`
- Create: `tests/test_v4l2_capture_state.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct V4l2StreamConfig {
    std::string node;
    int width;
    int height;
    int fps;
    unsigned buffer_count = 6;
};

struct V4l2CaptureStats {
    uint64_t acquired = 0;
    uint64_t sequence_gaps = 0;
    uint64_t poll_timeouts = 0;
};

class V4l2CaptureDevice {
public:
    explicit V4l2CaptureDevice(V4l2StreamConfig config);
    ~V4l2CaptureDevice();
    bool open_and_configure(std::string* error);
    bool start(std::string* error);
    bool read_frame(int timeout_ms, CompressedFrame* frame, std::string* error);
    void stop();
    const V4l2CaptureStats& stats() const;
};
```

- `read_frame` copies `bytesused` MJPEG bytes and performs `VIDIOC_QBUF` before
  returning.
- Partial initialization is safe to destroy or stop repeatedly.

- [ ] **Step 1: Write a failing format/state test**

Create an injected `V4l2CaptureOps` interface covering open, close, ioctl, poll,
mmap, and munmap. In `tests/test_v4l2_capture_state.cpp`, use a fake that records
operations and supplies six buffers.

Assert successful order:

```text
open → QUERYCAP → S_FMT → S_PARM → G_FMT → REQBUFS
→ QUERYBUF/mmap × 6 → QBUF × 6 → STREAMON
→ poll → DQBUF → copy → QBUF → STREAMOFF → munmap × 6 → close
```

Assert the copied frame remains valid after the fake driver buffer is mutated.

- [ ] **Step 2: Add target and verify RED**

Add `test_v4l2_capture_state` to `test` and `.PHONY`, compiling the test with
`v4l2_capture_device.cpp`.

Run `make test_v4l2_capture_state`.

Expected: compilation fails because the capture headers do not exist.

- [ ] **Step 3: Implement platform-neutral state and injected operations**

Define `V4l2CaptureOps` as a virtual interface. Production construction uses a
Linux implementation; tests pass a fake through an additional constructor:

```cpp
V4l2CaptureDevice(V4l2StreamConfig config,
                  std::unique_ptr<V4l2CaptureOps> ops);
```

Keep Linux-only headers in `v4l2_capture_device.cpp`.

- [ ] **Step 4: Implement negotiation and MMAP**

Require:

```cpp
format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
streamparm.parm.capture.timeperframe = {1, fps};
```

After `VIDIOC_G_FMT`, reject a pixel format, width, or height mismatch. Query
the actual frame interval and reject an fps mismatch beyond integer equality.

- [ ] **Step 5: Implement acquisition and statistics**

On every dequeued buffer:

- validate `index < mapped_buffers.size()`;
- validate `bytesused <= mapped_length`;
- detect sequence gaps after the first frame;
- convert `timeval` to microseconds;
- copy exactly `bytesused`;
- requeue before returning;
- increment acquired only after successful requeue.

Poll timeout is a nonfatal `false` return with an empty error string and an
incremented timeout count. Ioctl or validation failures return `false` with a
nonempty error.

- [ ] **Step 6: Add failure-cleanup tests**

For every initialization stage, configure the fake to fail and assert:

- all previously mapped buffers are unmapped;
- the fd is closed;
- `STREAMOFF` occurs only if streaming started;
- a second `stop()` performs no extra operations.

Add tests for wrong negotiated resolution, invalid buffer index, excessive
`bytesused`, poll timeout, and a sequence jump from 10 to 12.

- [ ] **Step 7: Verify and commit**

Run:

```bash
make test_v4l2_capture_state
make test
git diff --check
```

Commit:

```bash
git add hardware/video/v4l2_capture_device.* \
  hardware/video/v4l2_capture_ops.h tests/test_v4l2_capture_state.cpp Makefile
git commit -m "feat: add reusable V4L2 mmap capture device"
```

---

### Task 5: Migrate `VideoSensor` to V4L2 Acquisition

**Files:**
- Modify: `hardware/video/video_sensor.h`
- Modify: `app/session_runner.cpp`
- Create: `tests/test_video_pipeline_stats.cpp`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `V4l2CaptureDevice`, `CompressedFrameQueue`, and
  `CameraConfig::device_path`.
- Produces:

```cpp
struct VideoPipelineStats {
    uint64_t acquired;
    uint64_t processed;
    uint64_t decode_failures;
    uint64_t queue_overflows;
    uint64_t sequence_gaps;
};
```

- `VideoSensor` constructor no longer accepts a numeric device ID.

- [ ] **Step 1: Write the failing statistics test**

Create `tests/test_video_pipeline_stats.cpp` around a platform-neutral helper:

```cpp
VideoPipelineStats stats;
stats.acquired = 30;
stats.processed = 29;
stats.queue_overflows = 1;
assert(!video_pipeline_valid(stats));
stats.processed = 30;
stats.queue_overflows = 0;
assert(video_pipeline_valid(stats));
```

Validation returns false for any queue overflow, sequence gap, decode failure,
or acquired/processed mismatch.

- [ ] **Step 2: Verify RED**

Add the test target and run it.

Expected: compilation fails because `VideoPipelineStats` and
`video_pipeline_valid` do not exist.

- [ ] **Step 3: Add acquisition/processing separation**

In `collect()`:

1. start one processing thread;
2. in the sensor thread, repeatedly call `read_frame(2000, ...)`;
3. push the owned compressed frame to a queue with capacity `12`;
4. on shutdown, close the queue;
5. join the processing thread;
6. report all counters.

The processing thread contains the existing TurboJPEG, IMU queue, NV12, MPP,
Y8, and preview logic. It never calls V4L2 ioctls.

- [ ] **Step 4: Reuse processing buffers**

Allocate BGR and NV12 storage only when the first frame establishes the actual
resolution or when resolution changes. Replace per-frame:

```cpp
uint8_t* bgr = new uint8_t[bgr_size];
delete[] bgr;
```

with reusable `std::vector<uint8_t> bgr` and `std::vector<uint8_t> nv12`.

Preserve existing IMU and output behavior exactly.

- [ ] **Step 5: Replace setup and teardown**

Setup creates and configures `V4l2CaptureDevice` from `cfg_.device_path`,
`width`, `height`, and `fps`, then starts it after the existing JHH02 ordering
wait. Teardown calls `stop()`.

Remove all Nori types, calls, includes, and numeric IDs from `VideoSensor`.

- [ ] **Step 6: Update construction and verify**

Update `SessionRunner` calls to:

```cpp
std::make_unique<VideoSensor>(
    config, session_dir, session_number, session_timestamp,
    session_running_, capture_control_);
```

Run:

```bash
make test_video_pipeline_stats
make test
git diff --check
```

- [ ] **Step 7: Commit**

```bash
git add hardware/video/video_sensor.h app/session_runner.cpp \
  tests/test_video_pipeline_stats.cpp Makefile
git commit -m "feat: capture independent cameras through V4L2"
```

---

### Task 6: Migrate `SixCamSensor` and Remove Nori

**Files:**
- Modify: `hardware/video/sixcam_sensor.h`
- Modify: `app/session_runner.cpp`
- Modify: `app/runtime.cpp`
- Modify: `Makefile`
- Modify: `README.md`
- Modify: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: `CameraDiscoveryResult::sixcam.jhh02`,
  `CameraDiscoveryResult::sixcam.jhh04`, `V4l2CaptureDevice`, and
  `CompressedFrameQueue`.
- Produces: two ordered six-camera V4L2 acquisition/processing pipelines with
  independent statistics.

- [ ] **Step 1: Add a failing source-layout assertion**

Extend `tests/test_source_layout.sh`:

```sh
if rg -n 'Nori_Xvision|NORI_' app core hardware Makefile; then
    echo "production still references Nori SDK" >&2
    exit 1
fi
```

Run `make test_source_layout`.

Expected: FAIL and list current Nori references.

- [ ] **Step 2: Replace six-camera setup**

Construct one `V4l2CaptureDevice` per channel from its full `CameraConfig`.
Preserve start order inside `SixCamSensor::setup()`:

```text
configure both channels
start JHH02
mark jhh02_init_done
start JHH04
```

Do not derive either channel from numeric device ordering.

- [ ] **Step 3: Split acquisition and processing per channel**

For each channel, start:

- one acquisition thread that only reads V4L2 and pushes compressed frames;
- one ordered processing thread containing existing decode, IMU, MPP, Y8, and
  preview behavior.

Use queue capacity `12` per channel. Close queues before joining processors.
Report acquired, processed, overflow, sequence-gap, and decode-failure counters
per channel.

- [ ] **Step 4: Remove runtime Nori lifecycle**

Remove `Nori_Xvision_UnInit()` from every `Runtime::run` return path and from
normal shutdown. Remove the Nori include from `runtime.cpp`.

- [ ] **Step 5: Remove build dependency**

From `Makefile`:

- remove `-lNori_Xvision_Std`;
- remove `NORI_INC`, `NORI_LIB`, include path, library path, and rpath;
- add `hardware/video/v4l2_discovery.cpp` and
  `hardware/video/v4l2_capture_device.cpp` to `CPP_SOURCES`;
- update help text to describe V4L2 requirements.

Update README references from Nori device indices to V4L2 stable identity.

- [ ] **Step 6: Verify no Nori dependency remains**

Run:

```bash
rg -n 'Nori_Xvision|NORI_' app core hardware Makefile
make test_source_layout
make test
git diff --check
```

Expected: `rg` returns no matches; all tests exit `0`.

- [ ] **Step 7: Commit**

```bash
git add hardware/video/sixcam_sensor.h app/session_runner.cpp app/runtime.cpp \
  Makefile README.md tests/test_source_layout.sh
git commit -m "feat: remove Nori SDK from production capture"
```

---

### Task 7: Build and Validate on RK3588

**Files:**
- Create: `docs/records/2026-07-29-v4l2-capture-validation.md`

**Interfaces:**
- Consumes: completed V4L2 production binary.
- Produces: reproducible board evidence for device discovery, four-stream input,
  30-second processed output, frame counts, and output validity.

- [ ] **Step 1: Sync and build**

Run from the outer solution directory:

```bash
./tools/test_capture.sh build
```

Expected: board build exits `0`, and:

```bash
ssh root@192.168.100.200 \
  "ldd /usr/local/bin/unified_capture | grep -i Nori"
```

prints nothing.

- [ ] **Step 2: Verify scan identity**

Run:

```bash
ssh root@192.168.100.200 "/usr/local/bin/unified_capture --scan"
```

Require four capture nodes with VID/PID, product, serial, USB path, and exact
MJPEG formats. Save output in the validation record.

- [ ] **Step 3: Verify raw four-stream baseline**

Run four concurrent `v4l2-ctl` commands for 120 frames at:

```text
SL:    1440x960@30 MJPEG
JHHSW: 1440x960@30 MJPEG
JHH02: 4000x1200@30 MJPEG
JHH04: 3104x480@30 MJPEG
```

Require 120 frames per stream and approximately 30 fps after the first-frame
startup interval.

- [ ] **Step 4: Run the full pipeline for 30 seconds**

Run:

```bash
./tools/test_capture.sh -d 30
```

Require each enabled camera to report:

```text
acquired approximately 900
processed == acquired
queue_overflows == 0
sequence_gaps == 0
decode_failures == 0
```

Use a tolerance of ±2 frames for timeout boundary scheduling.

- [ ] **Step 5: Validate output artifacts**

For every configured H.265 camera:

```bash
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,avg_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 OUTPUT.mkv
```

Require `hevc` and 30 fps metadata.

For every Y8 output, require:

```text
file_size == width × height × processed_frames
```

For every IMU-enabled camera, require non-empty JSONL whose frame indices are
within acquired sequence bounds.

- [ ] **Step 6: Record evidence**

Create `docs/records/2026-07-29-v4l2-capture-validation.md` with:

```markdown
| Camera | Node | Serial | Acquired | Processed | Overflow | Gaps | Decode failures | MKV | Y8 | IMU |
|--------|------|--------|----------|-----------|----------|------|-----------------|-----|----|-----|
```

Include exact commands, exit codes, output directory, and any deviation from
900 frames.

- [ ] **Step 7: Run final verification and commit**

Run:

```bash
make test
git diff --check
git status --short
```

Commit:

```bash
git add docs/records/2026-07-29-v4l2-capture-validation.md
git commit -m "test: validate four-camera V4L2 capture"
```
