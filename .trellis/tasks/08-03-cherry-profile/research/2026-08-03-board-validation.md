# Cherry Profile Board Validation

Task 8 completed on `root@192.168.100.200`. The complete command transcript, exit statuses, first/last sample summaries, artifact sizes, ffprobe fields, synchronization statistics, caveats, and backup paths are committed on `feat/cherry-profile` in `docs/design/cherry-profile-board-validation.md` (`ec256e9`).

Key evidence:

- Host `make clean && make test`, deployment behavior test, Python synchronization tests, and `git diff --check`: exit 0.
- RK3588 `make clean && make test && make`: exit 0; production aarch64 link succeeded.
- Backups: `/etc/unified_capture/product.conf.bak.20260413_192111` and `/etc/unified_capture/camera-map.conf.bak.20260413_192111`.
- `/dev/video0` and `/dev/ttyACM0`: VID/PID `5268:1218`, shared canonical USB device parent `2-1.1`.
- Bounded session: `/media/usb0/capture/cherry_task8_20260413_192400/session_001/cherry_stereo`.
- MKV: H.264, 3200×1200, 30/1 fps, 27.666 s, 21,815,393 bytes.
- JSONL parse/counts: video frames 831, IMU 3,089, MAG 2,569, FRAME_META 8; every line parsed as JSON.
- Pipeline: acquired/processed 831/831, gaps 0, overflows 0, encoder failures 0, serial parser errors 0.
- Synchronization tool completed all three comparison pairs; exact percentiles are in the committed validation record.
- `unified_capture` systemd service was inactive before and after validation and was never started, stopped, or restarted.

Observed non-blocking caveats:

- The board clock showed 2026-04-13 while host files were dated 2026-08-03, producing future-mtime warnings.
- `--scan` prints only generic V4L2 inventory; the actual profile discovery log plus sysfs/udev evidence supplied the tty and USB-parent pairing details.
- GNU `timeout` returned 124 as expected after sending SIGINT. The application logged STOP, joined both sensors, and completed the session; FFmpeg reported 255 under the shared SIGINT, while the MKV remained fully probeable and countable.
