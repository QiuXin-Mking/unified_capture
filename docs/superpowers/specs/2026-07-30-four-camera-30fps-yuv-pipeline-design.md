# Four-Camera 30fps Direct-YUV Pipeline Design

## Context

The native V4L2 migration currently performs capture and processing in the
same per-camera loop. A dequeued V4L2 MMAP buffer remains owned by userspace
while TurboJPEG decodes MJPEG to BGR, the full BGR frame is copied to the IMU
queue, BGR is converted to NV12 in software, MPP encodes the frame, and output
is written. Channels with H.265 and Y8 disabled still perform the BGR decode
and NV12 conversion.

This caused the JHH02 failure to be misdiagnosed as an MPP encoder stop. A
controlled board test kept the session alive for about 30 seconds and produced
263 valid 4000x1200 H.265 frames. FFmpeg declared those frames as 30fps, so the
file duration was only 8.77 seconds even though the process never stopped.
The measured processing throughput was about 9.4fps, not a 7.5-second encoder
limit.

## Requirements

- Capture and process all four cameras at 30fps:
  - `wrist_left`: 1440x960 H.265 plus IMU;
  - `wrist_right`: 1440x960 H.265 plus IMU;
  - `jhh02`: 4000x1200 H.265 plus IMU;
  - `jhh04`: no video file, IMU only.
- JHH02 resolution must remain 4000x1200.
- Do not write JHH02 or JHH04 Y8 data to the SD card.
- Preserve the hardware-dependent stream-start order: JHH02 first, both wrist
  cameras after JHH02 is ready, then JHH04 after both wrists are ready.
- A successful validation has no V4L2 sequence gaps, processing-queue
  overflows, or decode failures.

## Selected Approach

Use a per-camera acquisition/processing pipeline and decode MJPEG directly to
planar YUV with TurboJPEG. The acquisition side owns V4L2 buffers for only the
time needed to copy compressed bytes. The processing side uses the decoded
luma plane for IMU extraction and packs planar chroma into stride-aligned NV12
for MPP.

This avoids the steady-state BGR frame, the CPU BGR-to-NV12 conversion, and the
full-frame BGR copy to the IMU queue. RGA remains a fallback rather than a new
runtime dependency because the direct-YUV path is simpler to test and removes
more memory traffic.

## Architecture

Each enabled camera has one ordered pipeline:

```text
V4L2 poll / DQBUF
        |
        +-- copy MJPEG bytes, timestamp, and sequence
        +-- QBUF immediately
        |
        v
bounded CompressedFrameQueue (capacity 12)
        |
        v
TurboJPEG direct planar-YUV decode
        |
        +-- luma plane -> IMU code-band extraction -> compact IMU record queue
        +-- planar Y/U/V -> stride-aligned NV12 -> MPP -> H.265 FIFO
        +-- JHH04 stops after IMU extraction
```

Acquisition never decodes, converts, encodes, or writes files. Processing is
single-threaded and ordered within each camera. The four cameras process in
parallel.

`VideoCaptureControl` counts JHH02 and both wrist cameras as prerequisites for
JHH04. JHH02 marks itself ready before either wrist stream starts; each wrist
decrements the prerequisite counter after `STREAMON`; JHH04 waits for the
counter to reach zero.

### Compressed frame ownership

`CompressedFrame` owns:

- monotonically increasing application frame index;
- V4L2 `sequence`;
- V4L2 timestamp converted to microseconds;
- copied MJPEG bytes.

`V4l2Device::dequeue_frame` returns a non-owning view with those metadata.
The acquisition loop copies the view into a reusable owned frame, immediately
requeues the driver buffer, then moves the owned frame into a bounded queue.

The queue supports non-blocking push, blocking pop, and explicit close.
Shutdown closes the queue after acquisition stops so the processor can drain
accepted frames and exit without polling sleeps.

### Direct YUV decode and NV12 packing

The processor reads the JPEG dimensions and subsampling from
`tjDecompressHeader3`, allocates reusable Y, U, V, and NV12 storage on the first
frame, and calls `tjDecompressToYUVPlanes`.

The NV12 packer:

- copies each visible Y row into a 64-byte-aligned output stride;
- interleaves U and V into the NV12 chroma plane;
- vertically downsamples 4:2:2 or 4:4:4 chroma to 4:2:0;
- horizontally downsamples 4:4:4 chroma to 4:2:0;
- directly interleaves 4:2:0 chroma without resampling;
- rejects unsupported or changing dimensions with a counted decode failure.

Padding bytes are initialized deterministically. MPP receives the negotiated
visible width and height plus the aligned NV12 stride.

### IMU extraction

The IMU code bands are black and white, and the existing decoder uses only the
green component as a brightness threshold. The decoder will instead accept a
one-byte luma plane and its row stride:

```cpp
uint32_t imu_read_luma_horizontal(
    const uint8_t* y, int width, int height, int stride, uint8_t* out);
uint32_t imu_read_luma_vertical(
    const uint8_t* y, int width, int height, int stride, uint8_t* out);
```

The video processor extracts the compact IMU payload immediately after YUV
decode and pushes only frame metadata plus at most 256 payload bytes to
`ImuSensor`. `ImuSensor` retains JSONL parsing and writing responsibility.
No full image is copied into the IMU queue.

### Output behavior

`SessionRunner` configures:

- wrist cameras with `output_h265=true`, `output_y8=false`;
- JHH02 with `output_h265=true`, `output_y8=false`;
- JHH04 with `output_h265=false`, `output_y8=false`.

A channel that needs only IMU performs YUV decode and luma extraction but does
not allocate or pack NV12. A channel with neither video output nor IMU only
acquires and requeues frames for stream-health accounting.

Preview remains on demand. When requested, the processing thread decodes that
one compressed frame to a scaled BGR preview; BGR is not produced during
normal capture.

## Statistics and Failure Visibility

Each pipeline reports:

```cpp
struct VideoPipelineStats {
    uint64_t acquired;
    uint64_t processed;
    uint64_t decode_failures;
    uint64_t queue_overflows;
    uint64_t sequence_gaps;
};
```

V4L2 sequence discontinuities, queue overflows, and decode failures are logged
with the camera name and included in the final summary. A queue overflow drops
the newly acquired compressed frame rather than blocking V4L2 requeue, but it
makes validation fail. MPP and FIFO write failures are surfaced separately and
also make the channel invalid.

The session remains controllable after an individual processing error. It
stops accepting frames for the failed channel, drains safely where possible,
and completes teardown without waiting forever on FFmpeg.

## Testing

Host tests cover:

- bounded compressed queue order, overflow, close, and drain behavior;
- V4L2 sequence-gap accounting including wraparound;
- planar 4:2:0, 4:2:2, 4:4:4, and grayscale-to-NV12 packing with padded stride;
- luma-based horizontal and vertical IMU decoding with synthetic code bands;
- pipeline validity rules;
- final camera output policy.

RK3588 validation runs the production configuration for at least 60 seconds.
It records per-camera acquired and processed counts and requires:

- at least 29.5 acquired and processed frames per second per camera after
  stream startup;
- zero queue overflows, sequence gaps, and decode failures;
- three valid HEVC MKV files at their required resolutions;
- no JHH02 or JHH04 Y8 file;
- non-empty IMU JSONL output for all four cameras;
- clean socket stop and FFmpeg teardown.

If direct-YUV processing still cannot sustain 30fps, timing counters around
TurboJPEG decode, chroma packing, MPP submission, and FIFO writing identify the
remaining stage. RGA conversion is considered only if measurements show NV12
packing, rather than JPEG decode or USB delivery, is the bottleneck.
