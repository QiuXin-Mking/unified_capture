# V4L2 Trigger Mode Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Identify the vendor UVC extension-unit transaction for trigger mode, use it to change all four cameras from mode `2` to mode `0`, and prove that every camera then streams MJPEG through V4L2.

**Architecture:** Keep the recovery experiment separate from production capture. First observe the vendor SDK at the syscall and USB-control boundaries, then build a generic command-line UVC XU probe whose unit, selector, size, and payload are explicit arguments. A SET request is permitted only when its exact format has been observed; successful recovery requires immediate GET readback and a V4L2 frame test.

**Tech Stack:** C++20, Linux V4L2/UVC (`UVCIOC_CTRL_QUERY`), `strace`, usbmon, `v4l2-ctl`, existing Nori SDK sample.

## Global Constraints

- Do not flash firmware.
- Do not overwrite the installed Nori SDK.
- Do not modify the production capture path during the experiment.
- Do not send guessed extension-unit payloads.
- Match devices using product and serial information, not `/dev/videoN` ordering.
- A successful SET must be followed by a GET that reports mode `0`.

---

### Task 1: Capture the Vendor Trigger Transaction

**Files:**
- Create: `docs/records/2026-07-29-nori-trigger-xu-trace.md`

**Interfaces:**
- Consumes: vendor `grab_image` executable and the four connected cameras.
- Produces: an evidence record containing the exact device node, extension-unit ID, selector, query type, payload size, and byte payload for GET mode and SET mode `0`.

- [ ] **Step 1: Record stable device identities and descriptors**

Run on RK3588:

```bash
v4l2-ctl --list-devices
for dev in /dev/video0 /dev/video2 /dev/video4 /dev/video6; do
  v4l2-ctl -d "$dev" --info
done
lsusb -v -d 1bcf:2d52
lsusb -v -d 1bcf:2d50
lsusb -v -d 1bcf:2d51
```

Record each product, serial, USB path, video node, extension-unit GUID, unit ID,
`bNumControls`, and `bmControls`.

- [ ] **Step 2: Trace the official sample while it requests mode `0`**

Run the vendor sample for SL device ID `0`, format index `1`:

```bash
cd /tmp
(printf '0\n1\n'; sleep 3; printf '\n') |
  strace -ff -yy -xx -s 512 \
    -e trace=openat,close,ioctl,read,write \
    -o /tmp/nori-trigger-strace \
    /root/pr-file/01-统一采集方案/新版sdk/sdk_extracted/Nori_Xvision_Development_Kit_Ver_20260629/linux_sdk/Samples/build/arm64/grab_image
```

Expected: trace files under `/tmp/nori-trigger-strace.*`. Preserve all ioctl
requests involving the selected `/dev/videoN` node.

- [ ] **Step 3: Capture USB control traffic for the same operation**

Resolve the SL bus number with `lsusb`, select the matching usbmon interface,
then run:

```bash
timeout 8 cat /sys/kernel/debug/usb/usbmon/2u > /tmp/nori-trigger-usbmon.txt
```

While the capture runs, execute the same official sample command from Step 2.
Filter `/tmp/nori-trigger-usbmon.txt` by the current SL device number and retain
the class-specific control transfers.

- [ ] **Step 4: Write the evidence record**

Create `docs/records/2026-07-29-nori-trigger-xu-trace.md` containing:

```markdown
# Nori Trigger XU Trace

## Device
- Product:
- Serial:
- USB path:
- Video node:
- Firmware:

## Extension Unit
- GUID:
- Unit ID:
- Selector:
- Payload size:

## Observed Requests
- GET request:
- GET response bytes:
- SET mode 0 request:
- SET mode 0 payload bytes:
- Immediate GET response bytes:

## Conclusion
- Whether the vendor SDK issued a UVC XU SET request.
- Whether the camera acknowledged and applied mode 0.
```

Every blank above must be replaced with an observed value or the explicit text
`not present in trace`; no value may be inferred.

- [ ] **Step 5: Commit the trace record**

```bash
git add docs/records/2026-07-29-nori-trigger-xu-trace.md
git commit -m "docs: record Nori trigger extension-unit transaction"
```

---

### Task 2: Build a Generic UVC XU Probe

**Files:**
- Create: `experiments/v4l2_trigger/xu_request.h`
- Create: `experiments/v4l2_trigger/trigger_xu_probe.cpp`
- Create: `tests/test_xu_request.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces: `XuRequest make_xu_request(uint8_t unit, uint8_t selector, uint8_t query, std::vector<uint8_t> payload)`.
- Produces: CLI `trigger_xu_probe DEVICE get UNIT SELECTOR SIZE`.
- Produces: CLI `trigger_xu_probe DEVICE set UNIT SELECTOR HEX_BYTES`.

- [ ] **Step 1: Write the failing request-construction test**

Create `tests/test_xu_request.cpp`:

```cpp
#include "experiments/v4l2_trigger/xu_request.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    const std::vector<uint8_t> payload{0x00, 0x01};
    const XuRequest request = make_xu_request(4, 2, 0x01, payload);
    assert(request.unit == 4);
    assert(request.selector == 2);
    assert(request.query == 0x01);
    assert(request.payload == payload);
    return 0;
}
```

- [ ] **Step 2: Add the test target and verify RED**

Add `test_xu_request` to `.PHONY` and `test`, with:

```make
test_xu_request: build/tests/test_xu_request
	./$<

build/tests/test_xu_request: tests/test_xu_request.cpp experiments/v4l2_trigger/xu_request.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I. -o $@ $<
```

Run:

```bash
make test_xu_request
```

Expected: compilation fails because `xu_request.h` does not exist.

- [ ] **Step 3: Implement the minimal platform-neutral request type**

Create `experiments/v4l2_trigger/xu_request.h`:

```cpp
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

struct XuRequest {
    uint8_t unit;
    uint8_t selector;
    uint8_t query;
    std::vector<uint8_t> payload;
};

inline XuRequest make_xu_request(uint8_t unit, uint8_t selector, uint8_t query,
                                 std::vector<uint8_t> payload) {
    return XuRequest{unit, selector, query, std::move(payload)};
}
```

Run `make test_xu_request`; expected: PASS.

- [ ] **Step 4: Write a failing CLI argument test**

Extend `xu_request.h` with declarations:

```cpp
bool parse_u8(const char* text, uint8_t* value);
bool parse_hex_bytes(const char* text, std::vector<uint8_t>* bytes);
```

Extend `tests/test_xu_request.cpp`:

```cpp
uint8_t value = 0;
assert(parse_u8("4", &value) && value == 4);
assert(!parse_u8("256", &value));
std::vector<uint8_t> bytes;
assert(parse_hex_bytes("00:01:ff", &bytes));
assert((bytes == std::vector<uint8_t>{0x00, 0x01, 0xff}));
assert(!parse_hex_bytes("0g", &bytes));
```

Run `make test_xu_request`; expected: link failure because the functions are
declared but undefined.

- [ ] **Step 5: Implement parsing and verify GREEN**

Implement both functions inline in `xu_request.h` using `strtoul`, rejecting
empty input, overflow beyond `255`, trailing characters, odd-length hex tokens,
and tokens outside `00` through `ff`.

Run:

```bash
make test_xu_request
make test
```

Expected: both commands exit `0`.

- [ ] **Step 6: Implement the Linux ioctl adapter**

Create `experiments/v4l2_trigger/trigger_xu_probe.cpp`. It must:

```cpp
// get: open DEVICE, allocate SIZE zero bytes, issue UVC_GET_CUR, print hex.
// set: parse HEX_BYTES, issue UVC_SET_CUR, then issue UVC_GET_CUR using the
//      same payload size and print the readback.
```

Use `open(..., O_RDWR)`, `uvc_xu_control_query`, `UVCIOC_CTRL_QUERY`,
`UVC_GET_CUR`, and `UVC_SET_CUR`. Reject SET unless all unit, selector, size,
and payload bytes were supplied explicitly.

Add:

```make
experiments/v4l2_trigger/trigger_xu_probe: experiments/v4l2_trigger/trigger_xu_probe.cpp experiments/v4l2_trigger/xu_request.h
	$(CXX) $(CXXFLAGS) -I. -o $@ $<
```

Compile on RK3588 after syncing the source. Expected: exit `0` and an executable
at `experiments/v4l2_trigger/trigger_xu_probe`.

- [ ] **Step 7: Commit the probe**

```bash
git add Makefile experiments/v4l2_trigger tests/test_xu_request.cpp
git commit -m "test: add generic V4L2 extension-unit probe"
```

---

### Task 3: Recover and Validate the Four Cameras

**Files:**
- Modify: `docs/records/2026-07-29-nori-trigger-xu-trace.md`

**Interfaces:**
- Consumes: the exact unit, selector, size, and payload observed in Task 1.
- Produces: evidence that all four cameras read mode `0` and stream 60 MJPEG frames.

- [ ] **Step 1: Prove GET-only behavior on SL**

Resolve the current SL capture node, then export the exact numeric values copied
from the completed Task 1 evidence record:

```bash
sl_node=
for node in /dev/video[0-9]*; do
  if udevadm info -q property -n "$node" | grep -q '^ID_SERIAL_SHORT=SL003$' &&
     v4l2-ctl -d "$node" --info 2>/dev/null | grep -q 'Video Capture'; then
    sl_node=$node
    break
  fi
done
test -n "$sl_node"

read -r -p 'Observed XU unit: ' xu_unit
read -r -p 'Observed XU selector: ' xu_selector
read -r -p 'Observed payload size: ' xu_size
read -r -p 'Observed mode-0 hex payload: ' mode0_hex
test -n "$xu_unit" -a -n "$xu_selector" -a -n "$xu_size" -a -n "$mode0_hex"
```

Enter only values copied from the observed Task 1 record. Then execute:

```bash
./experiments/v4l2_trigger/trigger_xu_probe \
  "$sl_node" get "$xu_unit" "$xu_selector" "$xu_size"
```

Expected: output bytes exactly match the SDK GET response. If they do not
match, stop without issuing SET.

- [ ] **Step 2: Set SL to mode `0` and verify readback**

Run:

```bash
./experiments/v4l2_trigger/trigger_xu_probe \
  "$sl_node" set "$xu_unit" "$xu_selector" "$mode0_hex"
```

Expected: the readback payload represents mode `0`. If readback remains mode
`2`, stop and preserve the result for Nori.

- [ ] **Step 3: Verify SL V4L2 streaming**

Run:

```bash
v4l2-ctl -d "$sl_node" \
  --set-fmt-video=width=1440,height=960,pixelformat=MJPG \
  --set-parm=30 --stream-mmap=4 --stream-count=60 \
  --stream-to=/dev/null --verbose
```

Expected: 60 frames complete without timeout and reported fps converges near 30.

- [ ] **Step 4: Repeat GET, SET, GET, and streaming for the other three cameras**

Resolve current nodes from product, serial, and USB path before every operation.
Use each camera's configured resolution:

```text
JHHSW SW002: 1440x960 MJPG 30
JHH 2d50:    4000x1200 MJPG 30
JHH 2d51:    3104x480 MJPG 30
```

Expected: mode `0` readback and 60 frames from every device.

- [ ] **Step 5: Record results and commit**

Append a validation table to the trace record:

```markdown
| Product | Serial | USB path | Video node | Before | SET result | After | 60-frame V4L2 |
|---------|--------|----------|------------|--------|------------|-------|----------------|
```

Run:

```bash
git diff --check
git add docs/records/2026-07-29-nori-trigger-xu-trace.md
git commit -m "docs: record V4L2 trigger recovery result"
```
