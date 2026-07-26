# Hardware Header Migration and Sampling Validation

## Scope

- Canonical IMU headers now live in `hardware/IMU/`:
  `imu_decode.h`, `ImuSensor.h`.
- Canonical camera pipeline headers now live in `hardware/VideoSensor/`:
  `VideoSensor.h`, `SixCamSensor.h`, `bgr2nv12.h`, `mpp_encoder.h`.
- Root-level copies of those six headers were removed. `main.cpp` and the
  Makefile only reference the hardware paths.

## Fixed Issue: Existing SD Output Directory Prevented Capture

The SD output policy creates `/media/usb0/capture` during startup. The old
`mkdir_p()` returned the final `mkdir()` result directly, so an existing
directory returned `EEXIST` and made `unified_capture` exit with status 2
before any sensor was initialized.

`mkdir_p()` now has `mkdir -p` semantics: an existing final path is accepted
only when `stat()` confirms it is a directory. `test_time_utils.cpp` covers
new nested-directory creation and a repeated call on the same path.

## RK3588 Sampling Evidence

Test session:

```
/media/usb0/capture/header_relocation_sampling_20260727_003/session_001
```

- Tracker global solve completed before stop.
- `tracker.jsonl`: T20 has 114 rows and T21 has 119 rows; every adjacent
  per-device timecode delta is exactly `480000` (48 MHz clock, 100 Hz).
- IMU JSONL rows / backward timestamps: jhh2_left `44/0`, jhh2_right `33/0`,
  jhh04 `165/0`, jhh02 `55/0`.
- Y8 files contain complete frames with zero trailing bytes: jhh2_left `5`,
  jhh2_right `4`, jhh04 `15`, jhh02 `5`.
- `jhh2_left.mkv`, `jhh2_right.mkv`, and `jhh02.mkv` completed full FFmpeg
  decode to `/dev/null` without errors.

## Open Issues

1. **libsurvive shutdown blocks after SIGINT.**
   The session logs `STOP`; all IMU, camera, and AS5600 threads finish; and
   the Tracker writes `tracker.jsonl` before the process blocks in
   `survive_close()`. The test process was terminated only after those files
   had closed. It does not reach `Session DONE` autonomously.

2. **Long full-load USB capture is not yet stable.**
   A previous longer test with all camera streams active reported T20 device
   disconnect, then a libusb mutex assertion. Kernel logs also showed
   `xhci-hcd ... couldn't access mem fast enough`. The short validation above
   intentionally stops immediately after Tracker global solve and does not
   prove long-duration stability.

## TODO

1. Isolate and bound the `survive_close()` shutdown path so SIGINT reaches
   `Session DONE` without a forced kill; preserve Tracker resampling before
   any bounded fallback.
2. Map the two Tracker USB devices and camera devices to physical ports;
   test Trackers on a dedicated controller or move the camera sharing their
   USB 2.0 hub.
3. Run a 30-second all-sensor SD-card test after steps 1–2. Acceptance:
   no Tracker disconnect/fatal assertion, automatic `Session DONE`, all IMU
   files non-empty, complete Y8/MKV files, and per-device Tracker deltas of
   `480000` throughout.
