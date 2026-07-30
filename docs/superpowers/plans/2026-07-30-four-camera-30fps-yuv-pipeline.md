# Four-Camera 30fps Direct-YUV Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sustain 30fps acquisition and processing for both wrist cameras, JHH02, and JHH04 while writing H.265 only for the wrists and JHH02 and preserving all four IMU streams.

**Architecture:** Each camera copies MJPEG from V4L2 and immediately requeues the driver buffer into a bounded owned-frame queue. A separate ordered processor decodes directly to planar YUV, extracts IMU from luma, and packs only H.265 channels to stride-aligned NV12 for MPP.

**Tech Stack:** C++20, Linux V4L2 MMAP, libjpeg-turbo, Rockchip MPP, FFmpeg, host-only C++ tests, RK3588 hardware validation.

## Global Constraints

- JHH02 remains 4000x1200 at 30fps.
- `wrist_left`, `wrist_right`, and `jhh02` write H.265 without Y8.
- `jhh04` writes neither H.265 nor Y8.
- All four cameras retain IMU JSONL output.
- Stream start order is JHH02, both wrists, then JHH04.
- Validation requires zero V4L2 sequence gaps, queue overflows, and decode failures.
- Preserve the untracked user record `docs/records/v4l2-30fps-jhh02-h265-status.md`.

---

### Task 1: Owned Compressed Frames, Bounded Queue, and Pipeline Statistics

**Files:**
- Create: `core/bounded_queue.h`
- Create: `hardware/video/compressed_frame_queue.h`
- Create: `hardware/video/video_pipeline_stats.h`
- Create: `tests/test_compressed_frame_queue.cpp`
- Create: `tests/test_video_pipeline_stats.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct CompressedFrame {
    uint64_t frame_idx = 0;
    uint64_t pts_us = 0;
    uint32_t v4l2_sequence = 0;
    std::vector<uint8_t> data;
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity);
    bool try_push(T&& value);
    bool wait_pop(T& value);
    void close();
    bool empty() const;
};

using CompressedFrameQueue = BoundedQueue<CompressedFrame>;

struct VideoPipelineStats {
    uint64_t acquired = 0;
    uint64_t processed = 0;
    uint64_t decode_failures = 0;
    uint64_t queue_overflows = 0;
    uint64_t sequence_gaps = 0;
    uint64_t encoder_failures = 0;
};

class V4l2SequenceTracker {
public:
    void observe(uint32_t sequence, VideoPipelineStats& stats);
};

bool video_pipeline_valid(const VideoPipelineStats& stats);
```

- [ ] **Step 1: Write failing queue and statistics tests**

Test FIFO order, rejection at capacity, draining after `close()`, `wait_pop`
returning false after a closed queue is empty, consecutive sequence numbers,
gaps, `UINT32_MAX` wraparound, and validity rejection for every error counter.

```cpp
CompressedFrameQueue queue(2);
assert(queue.try_push(CompressedFrame{1, 10, 7, {1}}));
assert(queue.try_push(CompressedFrame{2, 20, 8, {2}}));
assert(!queue.try_push(CompressedFrame{3, 30, 9, {3}}));
queue.close();
CompressedFrame out;
assert(queue.wait_pop(out) && out.frame_idx == 1);
assert(queue.wait_pop(out) && out.frame_idx == 2);
assert(!queue.wait_pop(out));
```

- [ ] **Step 2: Run the new tests and verify RED**

Run:

```bash
make test_compressed_frame_queue test_video_pipeline_stats
```

Expected: compilation fails because both headers and their interfaces are
missing.

- [ ] **Step 3: Implement the queue and statistics helpers**

Use one mutex and condition variable in `BoundedQueue`. `close()`
sets a boolean and wakes all waiters. `try_push()` rejects frames after close
or at capacity. `wait_pop()` waits for data or close and drains accepted frames
before returning false.

`V4l2SequenceTracker` treats unsigned increment as wrap-safe:

```cpp
const uint32_t expected = previous_ + 1;
if (sequence != expected) {
    stats.sequence_gaps += static_cast<uint32_t>(sequence - expected);
}
```

`video_pipeline_valid()` requires all five failure counters to be zero and
`acquired == processed`.

- [ ] **Step 4: Register and run both tests**

Add both targets to `Makefile` and the aggregate `test` target. Run:

```bash
make test_compressed_frame_queue test_video_pipeline_stats
```

Expected: both tests pass.

- [ ] **Step 5: Commit Task 1**

```bash
git add Makefile core/bounded_queue.h hardware/video/compressed_frame_queue.h \
  hardware/video/video_pipeline_stats.h \
  tests/test_compressed_frame_queue.cpp tests/test_video_pipeline_stats.cpp
git commit -m "feat(video): add bounded compressed frame pipeline"
```

---

### Task 2: V4L2 Frame Metadata and Immediate Requeue

**Files:**
- Create: `hardware/video/v4l2_frame_view.h`
- Modify: `hardware/video/v4l2_device.h`
- Create: `tests/test_v4l2_frame_view.cpp`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `CompressedFrame`.
- Produces:

```cpp
struct V4l2FrameView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t sequence = 0;
    uint64_t timestamp_us = 0;
};

bool V4l2Device::dequeue_frame(V4l2FrameView& frame);
bool V4l2Device::requeue_frame();

CompressedFrame copy_compressed_frame(
    const V4l2FrameView& view, uint64_t frame_idx, uint64_t fallback_pts_us);
```

- [ ] **Step 1: Write the failing frame-copy test**

Create a platform-neutral test that includes only `v4l2_frame_view.h` and uses
a small byte
array. Assert that bytes are owned after the source array changes, sequence is
preserved, and a zero driver timestamp uses `fallback_pts_us`.

- [ ] **Step 2: Verify RED**

Run:

```bash
make test_v4l2_frame_view
```

Expected: compilation fails because `V4l2FrameView` and
`copy_compressed_frame` do not exist.

- [ ] **Step 3: Implement metadata extraction**

Populate `V4l2FrameView` from `v4l2_buffer.bytesused`, `.sequence`, and
`.timestamp`. Validate `buf.index < buffers_.size()` and
`bytesused <= buffers_[buf.index].length`; log and reject invalid buffers.
Replace all old pointer/length dequeue call sites in later tasks, not in this
task.

- [ ] **Step 4: Run the focused and aggregate tests**

Run:

```bash
make test_v4l2_frame_view
make test
```

Expected: all host tests pass.

- [ ] **Step 5: Commit Task 2**

```bash
git add Makefile hardware/video/v4l2_frame_view.h \
  hardware/video/v4l2_device.h tests/test_v4l2_frame_view.cpp
git commit -m "feat(v4l2): retain frame sequence and timestamp"
```

---

### Task 3: Direct-YUV to NV12 Packing

**Files:**
- Create: `hardware/video/yuv_to_nv12.h`
- Create: `hardware/video/mjpeg_yuv_decoder.h`
- Create: `tests/test_yuv_to_nv12.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
enum class YuvSubsampling { yuv420, yuv422, yuv444, gray };

struct YuvPlaneView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

struct DecodedYuvFrame {
    int width = 0;
    int height = 0;
    YuvSubsampling subsampling = YuvSubsampling::gray;
    YuvPlaneView y;
    YuvPlaneView u;
    YuvPlaneView v;
};

bool pack_yuv_to_nv12(
    const DecodedYuvFrame& input, int nv12_stride,
    std::vector<uint8_t>& nv12);

class MjpegYuvDecoder {
public:
    bool decode(const uint8_t* jpeg, size_t size, DecodedYuvFrame& output);
    const char* error() const;
};
```

- [ ] **Step 1: Write failing packing tests**

Use synthetic planes to assert exact Y and interleaved UV bytes for:

- 4x4 4:2:0 direct packing;
- 4x4 4:2:2 vertical chroma averaging;
- 4x4 4:4:4 horizontal and vertical 2x2 averaging;
- grayscale with neutral U/V values of 128;
- an output stride larger than visible width with zeroed padding;
- odd dimensions rejected.

- [ ] **Step 2: Verify RED**

Run:

```bash
make test_yuv_to_nv12
```

Expected: compilation fails because `yuv_to_nv12.h` is missing.

- [ ] **Step 3: Implement the platform-neutral NV12 packer**

Resize output to `nv12_stride * height * 3 / 2`, initialize it to zero, copy
visible luma rows, and generate one U/V pair per 2x2 luma block. Use the mean
of the corresponding chroma samples when downsampling. Reject null planes,
odd visible dimensions, short strides, and an NV12 stride smaller than width.

- [ ] **Step 4: Implement the TurboJPEG decoder**

Use `tjDecompressHeader3`, `tjPlaneWidth`, `tjPlaneHeight`, and
`tjDecompressToYUVPlanes`. Reuse plane vectors until dimensions or
subsampling change. Map `TJSAMP_420`, `TJSAMP_422`, `TJSAMP_444`, and
`TJSAMP_GRAY`; reject other subsampling modes with a descriptive error.

- [ ] **Step 5: Verify the packer and board compilation**

Run:

```bash
make test_yuv_to_nv12
make test
scp hardware/video/mjpeg_yuv_decoder.h \
  root@192.168.100.200:/tmp/mjpeg_yuv_decoder.h
ssh root@192.168.100.200 \
  'printf "#include \"/tmp/mjpeg_yuv_decoder.h\"\nint main() { MjpegYuvDecoder d; }\n" |
   c++ -std=c++20 -Wall -I/root/pr-file/01-统一采集方案/unified_capture \
   -x c++ - -lturbojpeg -o /tmp/test_mjpeg_yuv_decoder'
```

Expected: host tests pass and the board compiler accepts TurboJPEG usage.

- [ ] **Step 6: Commit Task 3**

```bash
git add Makefile hardware/video/yuv_to_nv12.h \
  hardware/video/mjpeg_yuv_decoder.h tests/test_yuv_to_nv12.cpp
git commit -m "feat(video): decode MJPEG directly to NV12"
```

---

### Task 4: Luma IMU Extraction and Compact Queue Records

**Files:**
- Modify: `hardware/imu/imu_decode.h`
- Create: `hardware/imu/imu_frame_queue.h`
- Modify: `hardware/imu/imu_sensor.h`
- Create: `tests/test_imu_luma_decode.cpp`
- Create: `tests/test_imu_frame_queue.cpp`
- Modify: `Makefile`

**Interfaces:**
- Replaces the full-frame `BGRFrame` queue with:

```cpp
struct ImuFrame {
    uint64_t frame_idx = 0;
    uint64_t pts_us = 0;
    uint32_t size = 0;
    std::array<uint8_t, 256> data{};
};

using ImuFrameQueue = BoundedQueue<ImuFrame>;

uint32_t imu_read_luma_horizontal(
    const uint8_t* y, int width, int height, int stride, uint8_t* out);
uint32_t imu_read_luma_vertical(
    const uint8_t* y, int width, int height, int stride, uint8_t* out);
```

- [ ] **Step 1: Write failing luma and compact-queue tests**

Create synthetic horizontal and vertical black/white code bands with padded
luma strides. Assert the decoded bytes match the encoded payload. Assert an
`ImuFrame` stores no dynamically sized image vector and survives queue
push/pop unchanged.

- [ ] **Step 2: Verify RED**

Run:

```bash
make test_imu_luma_decode test_imu_frame_queue
```

Expected: compilation fails because the luma API and compact queue are absent.

- [ ] **Step 3: Implement luma decoding**

Change the line decoder to read one brightness byte per pixel. Horizontal
scanning addresses `y + row * stride`; vertical scanning copies
`y[row * stride + col]`. Reuse fixed `std::array` scratch buffers instead of
per-line `new[]`.

- [ ] **Step 4: Convert `ImuSensor` to compact records**

`ImuSensor` pops `ImuFrame`, calls `imu_parse_and_write` on the stored payload,
and never accesses image dimensions or pixels. Preserve JSONL format and final
summary counts.

- [ ] **Step 5: Run focused and aggregate tests**

Run:

```bash
make test_imu_luma_decode test_imu_frame_queue
make test
```

Expected: all tests pass.

- [ ] **Step 6: Commit Task 4**

```bash
git add Makefile hardware/imu/imu_decode.h \
  hardware/imu/imu_frame_queue.h hardware/imu/imu_sensor.h \
  tests/test_imu_luma_decode.cpp tests/test_imu_frame_queue.cpp
git commit -m "feat(imu): extract code bands from luma"
```

---

### Task 5: Shared Asynchronous Camera Processing

**Files:**
- Create: `hardware/video/video_frame_processor.h`
- Create: `hardware/video/capture_pipeline.h`
- Create: `tests/test_capture_pipeline.cpp`
- Modify: `hardware/video/mpp_encoder.h`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Tasks 1-4 helpers, `MppEncoder`, `ImuFrameQueue`, and
  `VideoCaptureControl`.
- Produces:

```cpp
struct VideoFrameProcessorOptions {
    const char* camera_name;
    bool output_h265;
    bool has_imu;
    ImuOrientation imu_orientation;
    int nv12_stride;
};

class VideoFrameProcessor {
public:
    bool process(const CompressedFrame& frame);
    const char* error() const;
};

template <typename CaptureSource, typename Processor>
VideoPipelineStats run_capture_pipeline(
    CaptureSource& source, std::atomic<bool>& running,
    Processor& processor, size_t queue_capacity = 12);
```

- [ ] **Step 1: Write the failing pipeline-order test**

Use a fake acquisition source and processor callback to prove that driver
frames are requeued before processing begins, processing order is preserved,
queue close drains accepted frames, and overflow increments the counter.

- [ ] **Step 2: Verify RED**

Run:

```bash
make test_capture_pipeline
```

Expected: compilation fails because `run_capture_pipeline` is missing.

- [ ] **Step 3: Implement acquisition/processing separation**

Start one processor thread. The calling acquisition thread waits for V4L2,
dequeues a view, updates sequence statistics, copies it, requeues immediately,
and moves it into the capacity-12 queue. On stop or fatal acquisition error,
close the queue and join the processor.

- [ ] **Step 4: Implement direct-YUV processing**

Decode the compressed frame once. Extract compact IMU payload from luma when
enabled. Pack and submit NV12 only for H.265 channels. Decode BGR only inside
the existing one-shot preview request branch.

Change `MppEncoder::put` to return a result that distinguishes an encoded
zero-byte frame from MPP submission or FIFO write failure:

```cpp
struct MppPutResult {
    bool ok = false;
    size_t bytes = 0;
};
```

Check every MPP return code, `fwrite`, and `ferror`.

- [ ] **Step 5: Add timing and final statistics**

Accumulate decode, IMU extraction, NV12 packing, MPP, and FIFO wall time in
microseconds. Emit one per-channel final line containing counters, measured
fps, and mean stage timings.

- [ ] **Step 6: Run focused and aggregate tests**

Run:

```bash
make test_capture_pipeline
make test
git diff --check
```

Expected: all tests pass.

- [ ] **Step 7: Commit Task 5**

```bash
git add Makefile hardware/video/video_frame_processor.h \
  hardware/video/capture_pipeline.h hardware/video/mpp_encoder.h \
  tests/test_capture_pipeline.cpp
git commit -m "feat(video): separate V4L2 acquisition from processing"
```

---

### Task 6: Integrate VideoSensor, SixCamSensor, and Final Output Policy

**Files:**
- Modify: `hardware/video/video_sensor.h`
- Modify: `hardware/video/sixcam_sensor.h`
- Modify: `hardware/video/capture_control.h`
- Modify: `app/session_runner.cpp`
- Modify: `tests/test_video_capture_control.cpp`
- Create: `tests/test_capture_output_policy.cpp`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `run_capture_pipeline`, `VideoFrameProcessor`, and compact
  `ImuFrameQueue`.
- Produces the final four-camera runtime configuration.

- [ ] **Step 1: Write failing start-order and output-policy tests**

Assert that banana stream prerequisites start with three devices (JHH02 and
two wrists), wrists cannot pass before `jhh02_init_done`, JHH04 cannot pass
until both wrists decrement the counter, and final output flags are:

```text
wrist_left  H265=true  Y8=false
wrist_right H265=true  Y8=false
jhh02       H265=true  Y8=false
jhh04       H265=false Y8=false
```

- [ ] **Step 2: Verify RED**

Run:

```bash
make test_video_capture_control test_capture_output_policy
```

Expected: at least the output-policy and banana prerequisite assertions fail.

- [ ] **Step 3: Replace synchronous sensor loops**

Both sensor classes retain setup, FFmpeg ownership, and teardown but delegate
their collect loops to the shared asynchronous pipeline. Remove BGR/NV12
per-frame allocation and the old `FrameQueue` use. Ensure JHH02 and JHH04 own
independent queues and processing threads.

- [ ] **Step 4: Correct stream-start coordination**

For banana, initialize control with two wrist prerequisites plus JHH02.
JHH02 starts and sets `jhh02_init_done`; wrist streams then start and decrement
the remaining count; JHH04 waits for zero before `STREAMON`.

- [ ] **Step 5: Apply final output policy**

Centralize the four output choices in a platform-neutral helper used by
`SessionRunner`, so the test verifies production values. Respect global
`--no-h265` only for profiles where it is allowed; banana continues requiring
the three H.265 streams.

- [ ] **Step 6: Run all host checks**

Run:

```bash
make test
git diff --check
```

Expected: every test passes and no whitespace errors are reported.

- [ ] **Step 7: Commit Task 6**

```bash
git add Makefile app/session_runner.cpp hardware/video/video_sensor.h \
  hardware/video/sixcam_sensor.h hardware/video/capture_control.h \
  tests/test_video_capture_control.cpp tests/test_capture_output_policy.cpp
git commit -m "fix(video): enable required four-camera output pipeline"
```

---

### Task 7: RK3588 Build, 60-Second Validation, and Status Record

**Files:**
- Modify: `docs/records/v4l2-30fps-jhh02-h265-status.md`
- Create: `docs/records/2026-07-30-four-camera-30fps-yuv-validation.md`

**Interfaces:**
- Consumes: completed production binary and final statistics logs.
- Produces reproducible evidence for the user-approved success criteria.

- [ ] **Step 1: Preserve and inspect board state**

Record the board repository status and back up only source files that the
validation deployment replaces. Do not reset or discard unrelated board
changes.

- [ ] **Step 2: Deploy exact local source and build**

Copy the changed source files to
`/root/pr-file/01-统一采集方案/unified_capture`, run `make -j4`, and retain the
complete build log.

- [ ] **Step 3: Run a controlled 60-second socket capture**

Launch:

```bash
./unified_capture --socket --no-as5600 validation_4cam_yuv
```

Send `start`, poll status and output sizes during the run, send `stop` after at
least 60 seconds of `running=true`, and then terminate the idle socket server
cleanly.

- [ ] **Step 4: Validate counters and files**

Require each camera to report at least 29.5 acquired and processed fps with
zero gaps, overflows, and decode failures. Use `ffprobe -count_frames` to
verify:

```text
wrist_left  1440x960 HEVC
wrist_right 1440x960 HEVC
jhh02       4000x1200 HEVC
```

Assert no JHH02 or JHH04 `.y8` file exists and each camera has a non-empty
JSONL file.

- [ ] **Step 5: Iterate only from measured bottleneck evidence**

If validation fails, use the stage timing counters to form one hypothesis,
write or extend a failing test, and change one stage at a time. Consider RGA
only when NV12 packing is the measured bottleneck.

- [ ] **Step 6: Update both records**

Rewrite the current-status record to correct the false 7.5-second MPP
diagnosis. Add commands, commit, board kernel, frame counts, rates, counter
values, FFprobe results, and remaining limitations to the validation record.

- [ ] **Step 7: Run completion verification**

Run:

```bash
make test
git diff --check
git status --short
```

Then rebuild once more on RK3588 and repeat a shorter smoke capture to ensure
the restored production configuration still starts and stops cleanly.

- [ ] **Step 8: Commit Task 7**

```bash
git add docs/records/v4l2-30fps-jhh02-h265-status.md \
  docs/records/2026-07-30-four-camera-30fps-yuv-validation.md
git commit -m "docs: record four-camera 30fps validation"
```
