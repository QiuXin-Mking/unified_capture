# Tracker Module Relocation Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the root-level legacy VIVE Tracker implementation with the RK3588-verified pose-only collector in `hardware/tracker/`, emitting independent fixed-100-Hz streams for every Tracker.

**Architecture:** `main.cpp` includes one focused `hardware/tracker/ViveTrackerSensor.h`. The sensor uses only libsurvive's pose callback (the successful `capture_vive` path), buffers records by `codename`, and resamples each device's own 48 MHz clock into `tracker.jsonl`; it never builds one grid from T20 and T21 timecodes. `tracker_raw.jsonl` retains every callback record.

**Tech Stack:** C++20, libsurvive, GNU Make, RK3588 Linux.

---

### Task 1: Add a testable fixed-grid helper

**Files:**

- Create: `hardware/tracker/resample_grid.h`
- Create: `hardware/tracker/test_resample_grid.cpp`
- Test: `hardware/tracker/test_resample_grid.cpp`

**Step 1: Write the failing test**

```cpp
const auto grid = make_resample_grid(1000, 1920999, 480000);
CHECK(grid == std::vector<uint64_t>({1000, 481000, 961000, 1441000}));
CHECK(make_resample_grid(20, 10, 480000).empty());
CHECK(make_resample_grid(10, 20, 0).empty());
```

**Step 2: Run it and verify failure**

Run: `g++ -std=c++20 -Wall -Werror -fsyntax-only hardware/tracker/test_resample_grid.cpp`

Expected: compilation fails because `resample_grid.h` does not yet exist.

**Step 3: Implement the helper**

```cpp
for (uint64_t tc = first;; tc += interval) {
    grid.push_back(tc);
    if (last - tc < interval) break;
}
```

Return an empty vector for zero interval or reverse range; use `last - tc < interval` to avoid unsigned overflow.

**Step 4: Verify green**

Run: `g++ -std=c++20 -Wall -Werror -Ihardware/tracker hardware/tracker/test_resample_grid.cpp -o /tmp/tracker_grid_test && /tmp/tracker_grid_test`

Expected: exit 0.

### Task 2: Move the verified sensor into `hardware/tracker/`

**Files:**

- Create: `hardware/tracker/ViveTrackerSensor.h`
- Modify: `main.cpp:30,334`
- Modify: `Makefile:51`
- Modify: `time_utils.h:17-24`
- Delete: `vive_tracker.h`
- Delete: `hardware/tracker/ViveTrackerSensor.cpp`
- Delete: `hardware/tracker/test_resample.cpp`
- Delete: `hardware/tracker/Makefile`

**Step 1: Write the failing integration check**

Create a 15-second RK3588-only test harness that instantiates `ViveTrackerSensor`, then validates `tracker.jsonl` by `codename`: each device must have at least two records and every consecutive `timecode` delta must equal `480000`.

Run: `make tracker_hardware_test`

Expected: target does not exist before the new module and test rule are added.

**Step 2: Implement the sensor contract**

Use this compatible constructor so the existing session call site needs no behavior change:

```cpp
ViveTrackerSensor(const std::string& session_dir, int session_num,
                  const std::string& session_ts, std::atomic<bool>& running);
```

Install only `survive_install_pose_fn`; do not call USB unbind helpers, force-calibration flags, angle callbacks, or light callbacks. The callback writes raw JSONL and appends `{timecode, ts_us, pose}` to `std::unordered_map<std::string, std::vector<Record>>`.

For every `codename`, sort its records and write a grid with `480000` ticks. Each output record has `ts_us`, `timecode`, `codename`, `method: "nearest"`, `x/y/z/qw/qx/qy/qz`. Write resampled data before `survive_close()`.

**Step 3: Repoint the production build**

```cpp
#include "hardware/tracker/ViveTrackerSensor.h"
```

Replace `vive_tracker.h` with `hardware/tracker/ViveTrackerSensor.h` in the `main.o` dependency rule. Keep the module header-only; do not add a new production object file.

**Step 4: Correct elapsed-time arithmetic**

Use signed `sec`/`nsec` subtraction and borrow one second when nanoseconds are negative, then return microseconds. This prevents the former unsigned nanosecond underflow seen in `ts_us`.

**Step 5: Verify local compilation surface**

Run: `make clean && make`

Expected: compilation reaches the platform SDK/link phase with no reference to root `vive_tracker.h` or the deleted tracker sources.

### Task 3: Verify the exact RK3588 behavior

**Files:**

- Create temporarily: `test_vive_tracker.cpp`
- Delete after verification: `test_vive_tracker.cpp`, `test_vive_tracker`

**Step 1: Build and run a controlled capture**

Run: `timeout -k 5 25 ./test_vive_tracker`

Expected: libsurvive discovers T20/T21 and the sensor reports a nonzero number of per-device 100-Hz frames.

**Step 2: Validate output per device**

Run an `awk` check that keys last timecode by `codename` and prints `bad_intervals=0` for each device.

Expected form:

```text
T20 frames=<positive> bad_intervals=0
T21 frames=<positive> bad_intervals=0
```

**Step 3: Remove the temporary harness and rebuild the production binary**

Run: `rm -f test_vive_tracker test_vive_tracker.cpp && make`

Expected: `unified_capture` builds and only the reusable module remains under `hardware/tracker/`.
