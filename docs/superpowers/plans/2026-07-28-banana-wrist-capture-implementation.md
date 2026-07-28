# Banana Wrist Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the `banana` product profile that records each discovered wrist camera as H.265 MKV and asynchronously writes its left-edge IMU JSONL stream, while preserving the existing `mango` topology.

**Architecture:** `Runtime` loads a small product-selection file and a separate camera map before Nori initialization.  `hardware/wrist` performs pure, testable left/right product-name matching and returns ordinary `CameraSlot` objects; `SessionRunner` reuses the existing `VideoSensor` plus `ImuSensor` pipeline with H.265 enabled, Y8 disabled, and `VERTICAL_LEFT` IMU orientation.  Banana supports an explicitly degraded session: available cameras record, missing slots remain visible in `status`, and a zero-camera session stays controllable until `stop`.

**Tech Stack:** C++20, Nori Xvision SDK, Rockchip MPP, FFmpeg FIFO muxing, libjpeg-turbo, POSIX Unix-domain socket, shell system tests.

## Global Constraints

- Product names are exactly `mango` and `banana`.
- Runtime defaults are `/etc/unified_capture/product.conf` and `/etc/unified_capture/camera-map.conf`; `--config <path>` overrides only the first path.
- Banana maps `wrist_left` to `SL` and `wrist_right` to `JHHSW` by exact Nori `iProduct`, never USB enumeration order.
- Banana requests MJPEG `1440x960@30` as input, encodes H.265 MKV, uses `ImuOrientation::VERTICAL_LEFT`, and never writes Y8.
- New headers expose declarations and small data structures only.  Parsing, matching, Nori SDK calls, and business logic belong in `.cpp` files.
- `allow_missing_devices=true` permits `start` for banana with one or zero discovered wrist cameras; malformed configuration still prevents service startup.
- Do not add microphone discovery, recording, muxing, or status in this change.
- Keep all host tests independent of Nori, MPP, FFmpeg, libsurvive, GPIO, and physical hardware.

---

## File Structure

| Path | Responsibility |
|---|---|
| `core/product_config.h` | Public product/profile and wrist-map value types; parser result interface. |
| `core/product_config.cpp` | Dependency-free key/value and INI parsing, validation, and human-readable errors. |
| `hardware/wrist/wrist_profile.h` | Public wrist format constants and typed slot-discovery value types. |
| `hardware/wrist/wrist_profile.cpp` | Creates `CameraConfig` values for left/right banana slots. |
| `hardware/wrist/wrist_discovery.h` | Public pure matching interface from an inventory to wrist slots. |
| `hardware/wrist/wrist_discovery.cpp` | Exact product matching, duplicate/missing/format diagnostics, degraded calculation. |
| `app/status_response.h` | Testable status-response value type and JSON serialization declaration. |
| `app/status_response.cpp` | JSON escaping and status JSON serialization. |
| `app/session_profile.h` | Dependency-free declaration for selecting active cameras from a product profile. |
| `app/session_profile.cpp` | Selects banana wrist slots or mango independent JHH2 slots for session setup. |
| `hardware/video/device_discovery.{h,cpp}` | Selects mango or banana discovery and adapts Nori SDK inventory for the pure wrist matcher. |
| `app/runtime.{h,cpp}`, `app/main.cpp` | Reads product configuration, enforces banana flags, permits degraded service startup, emits product-aware status. |
| `app/session_runner.{h,cpp}` | Starts only selected-profile cameras; supports banana zero-camera sessions and product-aware camera JSON. |
| `deploy/product.conf.example`, `deploy/camera-map.conf.example` | Installable configuration examples for `mango` and `banana`. |
| `tests/test_product_config.cpp` | Host coverage for the two config files and parser failures. |
| `tests/test_wrist_discovery.cpp` | Host coverage for exact left/right mapping and banana camera settings. |
| `tests/test_status_response.cpp` | Host coverage for product/degraded/camera JSON response shape. |
| `tests/test_session_profile.cpp` | Host coverage for banana and mango active-camera selection. |
| `tests/test_banana_wrist_socket.sh` | RK3588 acceptance for two, one, and zero wrist devices and H.265 output. |

## Task 1: Product configuration parser and deployment templates

**Files:**
- Create: `core/product_config.h`
- Create: `core/product_config.cpp`
- Create: `deploy/product.conf.example`
- Create: `deploy/camera-map.conf.example`
- Create: `tests/test_product_config.cpp`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces for all later tasks:

```cpp
enum class ProductProfile { mango, banana };

struct WristDeviceMap {
    bool allow_missing_devices = false;
    std::string left_product;
    std::string right_product;
};

struct ProductConfiguration {
    ProductProfile profile = ProductProfile::mango;
    WristDeviceMap wrist;
};

struct ProductConfigResult {
    std::optional<ProductConfiguration> configuration;
    std::string error;
};

ProductConfigResult load_product_configuration(
    const std::string& product_config_path,
    const std::string& camera_map_path);
std::string_view product_profile_name(ProductProfile profile);
```

- Consumes: no project production interface; use only `<fstream>`, `<optional>`, `<string>`, `<string_view>`, and standard containers.

- [ ] **Step 1: Write the failing parser test**

Create `tests/test_product_config.cpp`.  Use a unique directory under `std::filesystem::temp_directory_path()` and write these two files with `std::ofstream`:

```ini
# product.conf
product=banana
```

```ini
# camera-map.conf
[banana]
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW
head.enabled=false

[mango]
profile=legacy_head
```

Assert the parsed profile is `ProductProfile::banana`, degraded startup is true, and the two exact product strings are preserved.  Overwrite `product.conf` with `product=pear` and assert `configuration` is empty and `error` contains `unknown product`.  Overwrite the map with a banana section that omits `wrist_right.product` and assert parsing fails with an error mentioning that key.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test_product_config`

Expected: compilation fails because `core/product_config.h` and `load_product_configuration` do not exist.

- [ ] **Step 3: Add the public declarations and minimal parser**

Create `core/product_config.h` with the exact interface above.  In `core/product_config.cpp`:

1. trim leading/trailing ASCII whitespace;
2. ignore empty lines and lines beginning with `#` or `;`;
3. require exactly one non-comment `product=<value>` entry in the product file;
4. accept only `mango` and `banana`;
5. parse `[section]` and `key=value` entries in the map file;
6. for `banana`, require `allow_missing_devices`, `wrist_left.product`, and `wrist_right.product`, accepting only literal `true` or `false` for the boolean;
7. return an explanatory error instead of throwing for open, syntax, duplicate-key, or validation failures.

Use `product_profile_name()` to return the literal profile names for later status serialization.

- [ ] **Step 4: Add editable deployment templates**

Create `deploy/product.conf.example`:

```ini
product=banana
```

Create `deploy/camera-map.conf.example` with the approved `banana` and `mango` sections from the design.  Include comments identifying `/etc/unified_capture/` as the installed destination and stating that `SL`/`JHHSW` are editable exact `iProduct` matches.

- [ ] **Step 5: Wire and run the host test**

Add `core/product_config.cpp` to the production source lists in both build systems.  Add the Make target:

```make
test_product_config: build/tests/test_product_config

build/tests/test_product_config: tests/test_product_config.cpp core/product_config.cpp core/product_config.h

test: test_product_config
```

Compile the test with `tests/test_product_config.cpp core/product_config.cpp`, then run:

```bash
make test_product_config
git diff --check
```

Expected: the parser accepts the approved banana files and rejects both invalid fixtures; no whitespace errors are reported.

- [ ] **Step 6: Commit the parser slice**

```bash
git add core/product_config.h core/product_config.cpp deploy/product.conf.example \
  deploy/camera-map.conf.example tests/test_product_config.cpp Makefile CMakeLists.txt
git commit -m "feat: load capture product configuration"
```

## Task 2: Pure wrist profile and product-name matching

**Files:**
- Create: `hardware/wrist/wrist_profile.h`
- Create: `hardware/wrist/wrist_profile.cpp`
- Create: `hardware/wrist/wrist_discovery.h`
- Create: `hardware/wrist/wrist_discovery.cpp`
- Create: `tests/test_wrist_discovery.cpp`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: `ProductConfiguration`, `WristDeviceMap`, and `CameraConfig` from Task 1 and existing `core/camera_config.h`.
- Produces for Task 3:

```cpp
struct WristVideoFormat {
    bool is_mjpeg = false;
    int width = 0;
    int height = 0;
    int fps = 0;
};

struct WristDeviceInfo {
    uint32_t device_id = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string product;
    std::vector<WristVideoFormat> formats;
};

struct WristCameraSlot {
    CameraConfig config;
    bool available = false;
    std::string error;
};

struct WristDiscoveryResult {
    std::array<WristCameraSlot, 2> cameras;
    bool degraded = false;
    std::vector<std::string> errors;
    int active_count = 0;
};

WristDiscoveryResult match_wrist_cameras(
    const WristDeviceMap& device_map,
    const std::vector<WristDeviceInfo>& inventory);
```

- [ ] **Step 1: Write failing wrist matching tests**

Create `tests/test_wrist_discovery.cpp` with a helper that builds `WristDeviceInfo` values.  Cover these exact cases:

```cpp
const WristDeviceMap map{true, "SL", "JHHSW"};
const WristVideoFormat target{true, 1440, 960, 30};
```

1. one `SL` and one `JHHSW` with `target` produce two available slots named `wrist_left` and `wrist_right`;
2. both resulting `CameraConfig` values have `output_h265 == true`, `output_y8 == false`, and `imu_orientation == ImuOrientation::VERTICAL_LEFT`;
3. omitting `JHHSW` produces `active_count == 1`, `degraded == true`, and a right-slot error containing `missing`;
4. supplying two `SL` candidates leaves the left slot unavailable with an error containing `duplicate`;
5. supplying `SL` with only `1280x720@30` MJPEG leaves that slot unavailable with an error containing `1440x960@30`.

- [ ] **Step 2: Run the matching test to verify it fails**

Run: `make test_wrist_discovery`

Expected: compilation fails because the wrist profile and matching headers are absent.

- [ ] **Step 3: Implement the profile without Nori SDK dependencies**

In `wrist_profile.cpp`, construct static logical configurations:

```cpp
CameraConfig{"wrist_left", vid, pid, 0, 1440, 960, 30,
             8000000, 30, true, ImuOrientation::VERTICAL_LEFT,
             true, false, static_cast<int>(device_id)};
```

Construct the right configuration identically except for name, group order, VID/PID, and device ID.  Keep all construction logic in `.cpp`; the header contains only the small constants and declarations.

In `wrist_discovery.cpp`, collect candidates whose `product` equals each configured string exactly.  Require exactly one candidate and a format where `is_mjpeg`, width, height, and FPS equal the target.  Append each unavailability reason to both the slot and `errors`; set `degraded` when `allow_missing_devices` is true and at least one slot is unavailable.  Never assign an arbitrary duplicate candidate.

- [ ] **Step 4: Wire and run the host test**

Add both wrist `.cpp` files to the production source lists.  Add:

```make
test_wrist_discovery: build/tests/test_wrist_discovery

build/tests/test_wrist_discovery: tests/test_wrist_discovery.cpp \
  hardware/wrist/wrist_profile.cpp hardware/wrist/wrist_discovery.cpp

test: test_wrist_discovery
```

Add the four new wrist source files to the required-file list in `tests/test_source_layout.sh`.  Run:

```bash
make test_wrist_discovery
sh tests/test_source_layout.sh
git diff --check
```

Expected: all five matching scenarios pass and the source-layout check recognizes the new directory.

- [ ] **Step 5: Commit the wrist domain slice**

```bash
git add hardware/wrist tests/test_wrist_discovery.cpp tests/test_source_layout.sh \
  Makefile CMakeLists.txt
git commit -m "feat: add banana wrist device matching"
```

## Task 3: Product-aware Nori discovery

**Files:**
- Modify: `hardware/video/device_discovery.h`
- Modify: `hardware/video/device_discovery.cpp`
- Modify: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: `ProductConfiguration` from Task 1 and `WristDiscoveryResult` from Task 2.
- Produces for Tasks 4 and 5:

```cpp
struct CameraDiscoveryResult {
    ProductProfile profile = ProductProfile::mango;
    std::array<CameraSlot, 2> jhh2;
    SixCamDevices sixcam;
    std::array<CameraSlot, 2> wrist;
    bool degraded = false;
    std::vector<std::string> camera_errors;
    int active_count = 0;
};

CameraDiscoveryResult discover_cameras(const ProductConfiguration& configuration);
```

- [ ] **Step 1: Add a compile-time call-site failure**

Change the existing call in `app/runtime.cpp` to `discover_cameras(configuration)` before changing the discovery declaration.  Run:

```bash
make
```

Expected: the production compile fails because discovery still takes no configuration argument.  This is a compile-time red check; it must run on the RK3588 or configured cross-build environment because `make` links Nori and MPP.

- [ ] **Step 2: Add product-aware result fields and dispatch**

Update the header with the exact `CameraDiscoveryResult` shape above.  Split the existing body into an internal mango path that preserves current JHH2/SixCam matching and a banana path that:

1. calls `Nori_Xvision_Init` once;
2. builds `WristDeviceInfo` records from `DEVICE_INFO`, copying `iProduct` into a `std::string`;
3. uses `Nori_Xvision_GetDeviceVideoInfoSize` and `Nori_Xvision_GetDeviceVideoInfo` to populate every advertised format, mapping `VIDEO_MEDIA_TYPE_MJPG` to `is_mjpeg=true`;
4. calls `match_wrist_cameras(configuration.wrist, inventory)`;
5. copies the two returned camera configurations and availability flags into `result.wrist`, plus `degraded`, `camera_errors`, and `active_count`;
6. leaves `jhh2` and `sixcam` disabled for banana.

Keep `scan_devices()` independent of product configuration so it remains a raw hardware diagnostic when configuration files are absent or invalid.

- [ ] **Step 3: Check source layout and production compilation**

Run on the RK3588 target or configured cross-build environment:

```bash
make clean
make
make scan
sh tests/test_source_layout.sh
```

Expected: `make` links, `make scan` still prints raw Nori device data, and the layout check passes.  Do not claim camera matching is accepted until Task 6 runs with real wrist hardware.

- [ ] **Step 4: Commit product-aware discovery**

```bash
git add hardware/video/device_discovery.h hardware/video/device_discovery.cpp \
  app/runtime.cpp tests/test_source_layout.sh
git commit -m "feat: select camera discovery by product profile"
```

## Task 4: Product-aware runtime status and CLI configuration

**Files:**
- Create: `app/status_response.h`
- Create: `app/status_response.cpp`
- Create: `tests/test_status_response.cpp`
- Modify: `app/runtime.h`
- Modify: `app/runtime.cpp`
- Modify: `app/main.cpp`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ProductConfiguration`, `product_profile_name`, and `CameraDiscoveryResult`.
- Produces for Task 5:

```cpp
struct CaptureStatusResponse {
    std::string product;
    bool ready = false;
    bool degraded = false;
    bool running = false;
    long elapsed_ms = 0;
    std::vector<std::pair<std::string, bool>> cameras;
    std::vector<std::string> camera_errors;
    bool imu = false;
    bool as5600 = false;
    bool vive = false;
};

std::string make_capture_status_json(const CaptureStatusResponse& status);
```

- [ ] **Step 1: Write failing status serialization tests**

Create `tests/test_status_response.cpp` with this representative status:

```cpp
CaptureStatusResponse status{
    "banana", true, true, false, 0,
    {{"wrist_left", true}, {"wrist_right", false}},
    {"wrist_right: missing product JHHSW"}, true, false, false};
```

Assert the serialized JSON contains exactly one `"product":"banana"`, `"ready":true`, `"degraded":true`, both wrist keys and their booleans, and a `camera_errors` array.  Add an error containing a quote character and assert it is escaped as `\\\"` in the JSON response.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test_status_response`

Expected: compilation fails because `app/status_response.h` does not exist.

- [ ] **Step 3: Implement status serialization and CLI parsing**

In `status_response.cpp`, serialize fields in stable order: `ok`, `product`, `ready`, `degraded`, `running`, `session`, `elapsed_ms`, `cameras`, `camera_errors`, `imu`, `as5600`, `vive`.  Escape backslash, quote, newline, carriage return, and tab in all strings.

Extend `RuntimeOptions` with:

```cpp
std::string product_config_path = "/etc/unified_capture/product.conf";
std::string camera_map_path = "/etc/unified_capture/camera-map.conf";
```

Parse `--config <path>` in `main.cpp`; a missing path prints usage and returns `2` before creating `Runtime`.

At the start of `Runtime::run()`, after the `--scan` early return and before `discover_cameras`, call `load_product_configuration`.  Print its error and return `2` when no configuration is returned.  Reject banana plus `--no-h265` with return `2`, because banana's approved output is H.265 only.  For banana, disable AS5600 and VIVE in `SessionOptions`; for mango retain current option behavior.

For banana, retain the socket server even when `active_count == 0`; set `ready=true`, propagate `degraded`, and permit `start`.  For mango, preserve the existing no-camera failure.  Build both ready and running `status` responses through `make_capture_status_json`, always including product and only the selected profile's camera names.

- [ ] **Step 4: Run the host tests and source-level checks**

Add `app/status_response.cpp` to production sources and add:

```make
test_status_response: build/tests/test_status_response

build/tests/test_status_response: tests/test_status_response.cpp app/status_response.cpp app/status_response.h

test: test_status_response
```

Run:

```bash
make test_product_config test_wrist_discovery test_status_response
sh tests/test_source_layout.sh
git diff --check
```

Expected: all host assertions pass without Nori or MPP linkage, and no source-layout violation is reported.

- [ ] **Step 5: Commit runtime profile handling**

```bash
git add app/status_response.h app/status_response.cpp tests/test_status_response.cpp \
  app/runtime.h app/runtime.cpp app/main.cpp Makefile CMakeLists.txt
git commit -m "feat: report banana product and degraded status"
```

## Task 5: Banana session wiring and degraded zero-camera lifecycle

**Files:**
- Create: `app/session_profile.h`
- Create: `app/session_profile.cpp`
- Modify: `app/session_runner.h`
- Modify: `app/session_runner.cpp`
- Create: `tests/test_session_profile.cpp`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: Task 3 `CameraDiscoveryResult.profile`, `.wrist`, and Task 4 product-aware runtime state.
- Produces: the banana session behavior consumed by `Runtime::run()` and the exact `cameras_json()` fragment used by status serialization.

- [ ] **Step 1: Add a failing zero-camera session regression harness**

Create `app/session_profile.h` with this pure helper declaration so it can be host tested without Nori:

```cpp
std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras);
```

Create `tests/test_session_profile.cpp` with a banana result whose `wrist_left` is enabled and `wrist_right` is disabled.  Assert it returns exactly one left slot.  Repeat with both slots disabled and assert the vector is empty.  Add a mango result with one JHH2 slot enabled and assert it returns the mango slot, not wrist slots.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test_session_profile`

Expected: compilation fails because `active_profile_cameras` is undeclared and no target exists.

- [ ] **Step 3: Implement profile-specific session setup**

Implement `active_profile_cameras` in `session_profile.cpp`:

- for `ProductProfile::banana`, return enabled slots from `cameras.wrist` in left/right order;
- for `ProductProfile::mango`, return enabled independent JHH2 slots; retain the existing separate SixCam setup path.

In `make_session_dir`, create camera subdirectories only for `active_profile_cameras(cameras_)`; keep the existing SixCam directory creation only for mango.

In `run`, branch before the current mango setup:

1. banana calls `capture_control_.reset_stream_start(0, false)`;
2. for each active wrist slot, construct `VideoSensor` followed by `ImuSensor` with its configuration, preserving H.265 true and Y8 false;
3. banana does not add `EncoderSensor`, `ViveTrackerSensor`, or `SixCamSensor`;
4. mango executes the current JHH2/SixCam/AS5600/VIVE logic unchanged.

Replace the current `if (sensors.empty()) return;` with an empty-session control loop:

```cpp
if (sensors.empty()) {
    fprintf(stderr, "WARN: no active sensors in this session\n");
    while (session_running_) {
        if (pump) {
            pump(50);
        }
    }
    return;
}
```

This keeps a zero-device banana session responsive to socket `stop` instead of leaving `session_running_` stuck true.  Update `cameras_json()` to emit only `wrist_left` and `wrist_right` for banana, including both keys even when unavailable; preserve mango's current camera names.

- [ ] **Step 4: Add the host target and run regression tests**

Add this Make target, linking only the dependency-free selector implementation:

```make
test_session_profile: build/tests/test_session_profile

build/tests/test_session_profile: tests/test_session_profile.cpp app/session_profile.cpp app/session_profile.h

test: test_session_profile
```

Run:

```bash
make test_session_profile
make test
sh tests/test_source_layout.sh
git diff --check
```

Expected: banana active, partial, and zero-camera profile selection tests pass; the full host suite remains green.

- [ ] **Step 5: Commit session behavior**

```bash
git add app/session_profile.h app/session_profile.cpp app/session_runner.h \
  app/session_runner.cpp tests/test_session_profile.cpp Makefile CMakeLists.txt \
  tests/test_source_layout.sh
git commit -m "feat: run banana wrist capture sessions"
```

## Task 6: Build registration, documentation, and RK3588 acceptance script

**Files:**
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `tests/README.md`
- Create: `tests/test_banana_wrist_socket.sh`
- Modify: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: all production sources and status/output behavior from Tasks 1–5.
- Produces: repeatable host and board verification instructions for the approved banana feature.

- [ ] **Step 1: Write the failing documentation and acceptance-script assertions**

Create `tests/test_banana_wrist_socket.sh` with `set -euo pipefail`.  Require `PREFIX` and `SOCK` environment variables.  Before implementation is complete, make it fail with explicit checks for the future banana response fields:

```bash
status=$(printf 'status\n' | nc -U "$SOCK")
printf '%s\n' "$status" | grep -F '"product":"banana"'
printf '%s\n' "$status" | grep -F '"wrist_left"'
printf '%s\n' "$status" | grep -F '"wrist_right"'
```

Document the invocation in `tests/README.md`, including a two-device, one-device, and zero-device run.  Add README configuration-install instructions that copy the two `.example` files to `/etc/unified_capture/` and describe `mango`, `banana`, `degraded`, H.265 MKV, asynchronous IMU JSONL, no Y8, and the deferred microphone check.

- [ ] **Step 2: Run the script against the pre-feature service to verify it fails**

On an RK3588 deployment still running the pre-feature binary with a temporary banana configuration, run:

```bash
PREFIX=/media/usb0/capture/banana_precheck \
SOCK=/tmp/unified_capture.sock \
./tests/test_banana_wrist_socket.sh
```

Expected: it fails because the status response lacks `"product":"banana"`.  Do not overwrite a live deployed binary or user recordings to produce this red check.

- [ ] **Step 3: Complete the board acceptance script**

After Tasks 1–5, extend the script to:

1. assert two-device status has `"degraded":false`, then execute `start`, verify `"running":true`, execute `stop`, and verify `"running":false`;
2. locate each generated `wrist_left/*.mkv` and `wrist_right/*.mkv` under the new session directory and run `ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1`, requiring `hevc`;
3. require a non-empty `.jsonl` file for each connected wrist camera;
4. fail if `find "$session_dir" -name '*.y8' -print -quit` prints a path;
5. repeat with one wrist disconnected, require `"degraded":true`, allow start/stop, require only the connected side's MKV and JSONL;
6. repeat with both disconnected, require `"degraded":true`, require start/stop responses to succeed, and require no `.mkv`, `.jsonl`, or `.y8` below the empty session directory.

Use a timestamped `PREFIX` supplied by the caller; never delete output directories in the script.

- [ ] **Step 4: Register every source and test**

Ensure Make and CMake list every new production `.cpp`: `core/product_config.cpp`, `hardware/wrist/wrist_profile.cpp`, `hardware/wrist/wrist_discovery.cpp`, `app/status_response.cpp`, and `app/session_profile.cpp`.  Ensure `make test` runs `test_product_config`, `test_wrist_discovery`, `test_status_response`, `test_session_profile`, and `test_source_layout`.  Add all new required production headers and sources to `tests/test_source_layout.sh`.

- [ ] **Step 5: Run final host and target verification**

Host:

```bash
make clean
make test
sh tests/test_source_layout.sh
git diff --check
git status --short
```

RK3588 or configured cross-build environment:

```bash
make clean
make
./unified_capture --scan
PREFIX=/media/usb0/capture/banana_$(date +%Y%m%d_%H%M%S) \
SOCK=/tmp/unified_capture.sock \
./tests/test_banana_wrist_socket.sh
```

Expected: all host tests pass; the target build links; scan works; the board script proves H.265 MKV, JSONL output, no Y8, and degraded one/zero-device operation.

- [ ] **Step 6: Commit documentation and verification**

```bash
git add Makefile CMakeLists.txt README.md CLAUDE.md tests/README.md \
  tests/test_banana_wrist_socket.sh tests/test_source_layout.sh
git commit -m "docs: document banana wrist capture validation"
```
