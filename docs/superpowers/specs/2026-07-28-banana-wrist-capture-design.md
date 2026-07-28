# Banana wrist capture design

**Date:** 2026-07-28
**Status:** Approved for implementation
**Scope:** Add the `banana` product profile to `unified_capture` without changing the existing `apple` capture topology.

## Goal

Support a product with one left and one right wrist monocular camera.  Each connected wrist camera records an H.265 video stream in an MKV container and emits its own IMU JSONL stream.  IMU decoding must not run in the video capture path, and the wrist profile must not create Y8 files.

The current hardware has only the two wrist cameras.  The design reserves a future head-camera entry but must work when either or both wrist cameras are absent.

## Confirmed decisions

- The selected product is read before Nori SDK initialization.
- `/etc/unified_capture/product.conf` selects either `apple` or `banana` with a single `product=<name>` entry.
- `/etc/unified_capture/camera-map.conf` is a separate, editable mapping from product profile and logical camera name to Nori `iProduct` value.
- `apple` retains the existing independent JHH2 and SixCam behavior.
- `banana` records H.265 MKV, not raw MJPEG MKV.
- The source camera delivers MJPEG.  The camera thread decodes it to BGR once, encodes the BGR-derived NV12 to H.265, and puts a bounded BGR copy on a separate IMU queue.
- Wrist IMU code is located at the left edge of the image and uses `ImuOrientation::VERTICAL_LEFT`.
- No wrist Y8 output is created.
- A degraded profile accepts `start` even with one or both configured wrist devices absent.

## Configuration

### Product selection

`/etc/unified_capture/product.conf`:

```ini
product=banana
```

The runtime also supports `--config <path>` for development and test deployments.  This override replaces only the product-selection file path; the device map remains `/etc/unified_capture/camera-map.conf`.

### Device map

`/etc/unified_capture/camera-map.conf` initially contains:

```ini
[banana]
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW
head.enabled=false

[apple]
profile=legacy_head
```

The names `SL` and `JHHSW` are initial editable defaults.  Discovery treats them as exact Nori `iProduct` matches, never as USB enumeration order.  A future deployment can change only this file to exchange devices or rename either side.

The banana profile uses the wrist script's current format target of MJPEG `1440x960@30`.  Its H.265 bitrate and GOP are profile defaults held with the wrist profile implementation.  Discovery must verify that the selected device exposes the requested input format before marking the slot available.

## Components and boundaries

```text
product.conf + camera-map.conf
        |
        v
ProductConfig ----> Runtime ----> selected product profile
                                      |
               +----------------------+------------------+
               |                                         |
             apple                                     banana
          existing discovery                hardware/wrist discovery
                                                     |
                                            wrist_left / wrist_right slots
                                                     |
                       +-----------------------------+-----------------------------+
                       |                                                           |
                 VideoSensor                                                ImuSensor
           MJPEG -> BGR -> NV12 -> H.265                          bounded FrameQueue ->
           MPP -> FFmpeg -> MKV                                      VERTICAL_LEFT -> JSONL
```

New files under `hardware/wrist/`:

- `wrist_profile.h` / `wrist_profile.cpp` define the banana profile's public data model and construct its camera slot configurations.
- `wrist_discovery.h` / `wrist_discovery.cpp` enumerate Nori devices and assign left/right wrist slots using the map file.

Those headers contain only declarations and small data structures.  Parsing, Nori enumeration, device matching, format validation, and error formatting are implemented in `.cpp` files.

`VideoSensor` and `ImuSensor` remain the shared video/IMU pipeline rather than copying the encoder.  Banana passes `output_h265=true`, `output_y8=false`, and `VERTICAL_LEFT` to those components.  Its stream-start control declares that no SixCam JHH02 dependency exists, so the two wrist sensors can begin together.

`core/product_config.h` / `core/product_config.cpp` own the small dependency-free INI parser and validation.  They expose the selected product, the degraded-start policy, and a typed map needed by profile discovery.  No configuration parsing is performed by `main.cpp` or sensor classes.

## Session lifecycle and output

For a `banana` start request, `SessionRunner` creates a `VideoSensor` and an `ImuSensor` only for each discovered wrist slot.  Both live sensor threads participate in the existing session barrier.  `ImuSensor` drains its queue after recording stops before closing its JSONL file.

For each active device, session output is:

```text
session_001/
├── wrist_left/
│   ├── wrist_left-<timestamp>.mkv
│   └── wrist_left-<timestamp>.jsonl
└── wrist_right/
    ├── wrist_right-<timestamp>.mkv
    └── wrist_right-<timestamp>.jsonl
```

The MKV is a Rockchip-MPP H.265 elementary stream passed through a FIFO to FFmpeg for MKV muxing.  It contains no raw MJPEG and no Y8 sidecar.  The IMU queue is deliberately bounded and non-blocking: when it is full, the BGR candidate frame is dropped for IMU only and the H.265 recording continues.  JSONL therefore need not have one record per video frame.

## Status and degraded operation

The `status` response keeps the existing fields and adds `product`, `degraded`, and optional `camera_errors`.  Banana reports only active-profile camera keys:

```json
{
  "ok": true,
  "product": "banana",
  "ready": true,
  "degraded": true,
  "running": false,
  "cameras": {
    "wrist_left": true,
    "wrist_right": false
  }
}
```

`degraded` is true when one or more configured camera slots are unavailable and `allow_missing_devices=true`.  In that state `start` is accepted unconditionally:

- available wrist cameras record normally;
- missing cameras create no sensor, directory, or empty output;
- if neither wrist camera is available, the service still accepts `start` and `stop`, exposes its status, and creates no video output.

Duplicate matches and unsupported formats make the affected logical slot unavailable rather than assigning a potentially wrong camera.  They are reported through `camera_errors` and do not prevent other available slots from recording when degraded operation is enabled.

Missing or malformed product configuration, an unknown product, or an invalid camera-map file prevents service startup.  The legacy Apple profile remains strict unless its own map section explicitly enables degraded operation.

## Error isolation

- Failure to initialize H.265, create a FIFO, or start FFmpeg affects only the associated wrist sensor.  The opposite wrist sensor remains running.
- IMU decoder failures and queue overflow never block video capture.  They are counted and logged per camera.
- At stop, each active video sensor flushes MPP, closes its FIFO, and waits for its FFmpeg child before the session completes.  The corresponding IMU thread drains already-enqueued frames before closing JSONL.

## Verification

Host-side tests added to `make test` cover:

1. product selection parsing, including unknown and malformed values;
2. banana map parsing and exact left/right `iProduct` matching;
3. missing and duplicate inventory cases, including `degraded=true` behavior;
4. banana `status` JSON with only `wrist_left` and `wrist_right` camera keys;
5. banana camera configuration: H.265 enabled, Y8 disabled, and left-edge IMU orientation selected.

RK3588 acceptance requires:

1. boot the service with two, one, and zero wrist cameras attached and inspect `status`;
2. issue `start`, `status`, and `stop` in the two-camera and one-camera cases;
3. run `ffprobe` on each MKV and verify an H.265 video stream;
4. verify the matching per-wrist JSONL files exist and contain decodable IMU records;
5. verify no `.y8` file is written for banana;
6. confirm the missing-side case remains startable and records only the connected side.

## Non-goals

- Do not attach banana head cameras in this change.
- Do not replace or rewrite Apple camera discovery, SixCam sequencing, AS5600, or VIVE behavior.
- Do not add a raw MJPEG recording format or offline IMU decode phase for banana.
