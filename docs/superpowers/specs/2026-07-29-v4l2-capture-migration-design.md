# V4L2 Capture Migration Design

**Date:** 2026-07-29

## Goal

Replace Nori Xvision SDK video discovery and streaming with native Linux V4L2
while preserving the existing banana and mango profiles, output layout, H.265,
Y8, IMU, preview, session control, and camera start ordering.

The banana acceptance target is four simultaneous cameras for at least 30
seconds, approximately 900 frames per camera, with no silent frame drops.

## Evidence

- The Nori SDK reports all cameras in trigger mode `2`, and
  `SetTriggerMode(0)` returns success without changing the readback.
- The vendor arm64 `grab_image` sample reproduces the same SL zero-frame
  behavior.
- After reconnecting the cameras, native V4L2 captured 120 frames from all four
  cameras concurrently.
- Between frame 20 and frame 120, every camera delivered approximately 100
  frames in 3.328 seconds, or about 30 fps.
- `/dev/videoN` assignments changed after reconnecting the devices.

## Architecture

### Device discovery

Enumerate `/dev/video*` capture nodes and use `VIDIOC_QUERYCAP` to reject
metadata-only nodes. Resolve the USB parent through sysfs and read:

- vendor ID;
- product ID;
- USB product string;
- serial number;
- bus number and physical USB path.

Reuse the existing profile matching rules:

- wrist-left: product `SL`;
- wrist-right: product `JHHSW`;
- six-camera JHH02: VID/PID `1bcf:2d50`;
- six-camera JHH04: VID/PID `1bcf:2d51`, paired by USB topology.

Device identity must never depend on the current `/dev/videoN` number.

### V4L2 stream lifecycle

Each camera owns a `V4l2CaptureDevice` with this lifecycle:

1. open the selected node with `O_RDWR | O_NONBLOCK`;
2. query capabilities;
3. request MJPEG with the configured width, height, and fps;
4. verify the negotiated format;
5. request and mmap multiple driver buffers;
6. queue every buffer;
7. call `VIDIOC_STREAMON`;
8. use `poll` followed by `VIDIOC_DQBUF`;
9. copy only the compressed MJPEG bytes into an owned frame;
10. immediately call `VIDIOC_QBUF`;
11. call `VIDIOC_STREAMOFF`, unmap buffers, and close during teardown.

The driver timestamp and sequence number are retained with every compressed
frame.

### Acquisition and processing

The capture thread may not decode, convert, encode, or write files while it
owns a V4L2 buffer.

```text
V4L2 poll/DQBUF
    |
    +-- copy compressed MJPEG into reusable owned storage
    |
    +-- QBUF immediately
    |
    v
bounded per-camera processing queue
    |
    +-- TurboJPEG decode
    +-- IMU extraction
    +-- BGR to NV12
    +-- MPP H.265
    +-- Y8 / JSONL / MKV output
```

Processing stays ordered per camera. Buffers and frame storage are reused after
the initial allocation. Queue overflow is counted and reported as a capture
failure; it must not be silently ignored.

### Camera ordering

Preserve the current hardware-dependent order:

1. initialize and start six-camera JHH02;
2. initialize and start six-camera JHH04;
3. initialize and start wrist cameras;
4. release the session barrier after all enabled streams are ready.

This ordering remains independent of `/dev/videoN` enumeration order.

### Nori SDK removal

Remove these operations from production:

- `Nori_Xvision_Init`;
- `Nori_Xvision_GetDeviceInfo`;
- `Nori_Xvision_DeviceVideoInit`;
- `Nori_Xvision_SetTriggerMode`;
- `Nori_Xvision_VideoStart` and `VideoStop`;
- `Nori_Xvision_GetFrameBuff` and `FreeFrameBuff`;
- `Nori_Xvision_DeviceVideoUnInit`;
- `Nori_Xvision_UnInit`.

Remove the Nori include path, library path, runtime rpath, and
`-lNori_Xvision_Std` from the production build after no source file references
the API.

## Components

### `hardware/video/v4l2_device`

Owns node discovery metadata, V4L2 ioctl calls, mmap buffers, stream state, and
compressed-frame acquisition. It exposes explicit error results and guarantees
cleanup on partial initialization.

### `hardware/video/v4l2_discovery`

Enumerates capture nodes and converts sysfs/udev information into the existing
camera inventory and profile-selection inputs.

### `hardware/video/compressed_frame_queue`

Provides bounded, ordered ownership transfer between capture and processing.
It records high-water mark and overflow count and wakes consumers during
shutdown.

### Existing sensors

`VideoSensor` and `SixCamSensor` retain output and IMU responsibilities but
consume owned compressed frames rather than Nori frame pointers.

## Error Handling

- Failure to open, negotiate, mmap, or start any required camera is a setup
  failure with the node, product, serial, ioctl name, and `errno` in the log.
- A negotiated format different from the requested MJPEG resolution or fps is
  rejected.
- Poll timeout reports the camera identity and elapsed time.
- Invalid V4L2 buffer index or byte count stops that stream safely.
- Queue overflow increments a counter and makes the final session result fail.
- `allow_missing_devices=true` continues to control discovery degradation; it
  does not hide failures for an enabled camera.

## Testing

### Host tests

- sysfs/device metadata matching independent of `/dev/videoN`;
- exclusion of metadata-only nodes;
- wrist product and six-camera topology mapping;
- compressed-frame queue ordering, shutdown, overflow, and counters;
- V4L2 state-machine cleanup through an injected ioctl adapter;
- negotiated-format rejection.

### RK3588 tests

1. Run each camera alone for 60 frames.
2. Run all four V4L2 streams concurrently for 120 frames.
3. Run `unified_capture` for at least 30 seconds.
4. Require approximately 900 acquired and processed frames per enabled camera.
5. Require zero queue overflow and no V4L2 sequence gaps.
6. Validate H.265 MKV with `ffprobe`.
7. Validate Y8 sizes from width × height × processed-frame count.
8. Validate non-empty, frame-indexed IMU JSONL for every IMU-enabled camera.

## Out of Scope

- Firmware updates.
- Reverse-engineering or writing vendor UVC extension-unit controls.
- Hardware-trigger synchronization.
- Changing output formats or product configuration syntax.

