# unified_capture 按硬件拆分 .h/.cpp 计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 unified_capture 按物理硬件设备拆分为独立文件，一个硬件一个 .h。

**Architecture:** 每个物理设备 = 一个 Sensor 子类 = 一个独立头文件。VideoSensor → ImuSensor 通过 FrameQueue 通信，只拆文件不改变逻辑。

**Tech Stack:** C++20, TSTC SDK, Rockchip MPP, libgpiod, libsurvive

---

### 拆分原则

```
一个物理设备 → 一个 .h 文件 → 一个 Sensor 子类

TSTC USB 摄像头    → video_sensor.h      (VideoSensor + MppEncoder)
ICM42688 IMU       → imu_sensor.h        (ImuSensor)  ← 从 video_sensor.h 拆出
AS5600 磁编码器     → encoder_sensor.h    (EncoderSensor)
VIVE Tracker 3.0   → vive_tracker.h      (ViveTrackerSensor)
```

### 基础设施抽离

`sensor.h` 和 `video_sensor.h` 各塞了太多东西，先拆出来：

| 抽出内容 | 新建文件 | 来源 |
|----------|----------|------|
| CameraConfig + ImuOrientation | `camera_config.h` | video_sensor.h |
| elapsed_us + mkdir_p + g_t0 | `time_utils.h` | sensor.h |
| SimpleBarrier | `barrier.h` | sensor.h |
| BGRFrame + FrameQueue | `frame_queue.h` | sensor.h |
| bgr_to_nv12 | `bgr2nv12.h` | video_sensor.h |
| MppEncoder | `mpp_encoder.h` | video_sensor.h |
| ImuSensor 类 | `imu_sensor.h` | video_sensor.h |
| USB unbind/rebind | `vive_usb.h` | vive_tracker.h |

### 拆分后依赖关系

```
main.cpp
  ├── camera_config.h     (纯结构体，无依赖)
  ├── barrier.h           (纯类，无依赖)
  ├── sensor.h            (依赖 time_utils.h)
  ├── video_sensor.h      (依赖 sensor.h + mpp_encoder.h + bgr2nv12.h + frame_queue.h)
  ├── imu_sensor.h        (依赖 sensor.h + imu_decode.h + frame_queue.h)
  ├── encoder_sensor.h    (依赖 sensor.h)
  └── vive_tracker.h      (依赖 sensor.h + vive_usb.h)
```

---

### Task 1: 抽离 `camera_config.h`

**Files:**
- Create: `camera_config.h`
- Modify: `video_sensor.h` (删 CameraConfig + ImuOrientation, 加 `#include "camera_config.h"`)
- Modify: `main.cpp` (加 `#include "camera_config.h"`)

从 `video_sensor.h:187-202` 搬出 `enum class ImuOrientation` 和 `struct CameraConfig`，替换为 include。

**Commit:** `refactor: extract camera_config.h from video_sensor.h`

---

### Task 2: 抽离 `time_utils.h`

**Files:**
- Create: `time_utils.h`
- Modify: `sensor.h` (删 g_t0 声明 + elapsed_us + mkdir_p, 加 `#include "time_utils.h"`)

从 `sensor.h` 搬出 `extern struct timespec g_t0`、`elapsed_us()`、`mkdir_p()`。

**Commit:** `refactor: extract time_utils.h from sensor.h`

---

### Task 3: 抽离 `barrier.h`

**Files:**
- Create: `barrier.h`
- Modify: `sensor.h` (删 SimpleBarrier, 加 `#include "barrier.h"`)

从 `sensor.h:50-73` 搬出 `class SimpleBarrier`。

**Commit:** `refactor: extract barrier.h from sensor.h`

---

### Task 4: 抽离 `frame_queue.h`

**Files:**
- Create: `frame_queue.h`
- Modify: `sensor.h` (删 BGRFrame + FrameQueue, 加 `#include "frame_queue.h"`)
- Modify: `video_sensor.h` (加 `#include "frame_queue.h"`)

从 `sensor.h:113-172` 搬出 `struct BGRFrame` 和 `class FrameQueue`。

**Commit:** `refactor: extract frame_queue.h from sensor.h`

---

### Task 5: 抽离 `bgr2nv12.h`

**Files:**
- Create: `bgr2nv12.h`
- Modify: `video_sensor.h` (删 bgr_to_nv12, 加 `#include "bgr2nv12.h"`)

从 `video_sensor.h:37-56` 搬出 `bgr_to_nv12()`。

**Commit:** `refactor: extract bgr2nv12.h from video_sensor.h`

---

### Task 6: 抽离 `mpp_encoder.h`

**Files:**
- Create: `mpp_encoder.h`
- Modify: `video_sensor.h` (删 MppEncoder struct, 加 `#include "mpp_encoder.h"`)

从 `video_sensor.h:61-180` 搬出 `struct MppEncoder`。

**Commit:** `refactor: extract mpp_encoder.h from video_sensor.h`

---

### Task 7: 拆 `imu_sensor.h`

**Files:**
- Create: `imu_sensor.h`
- Modify: `video_sensor.h` (删 ImuSensor class)
- Modify: `main.cpp` (加 `#include "imu_sensor.h"`)

从 `video_sensor.h:474-542` 搬出 `class ImuSensor`。

ImuSensor 需要的 include:
```cpp
#include "sensor.h"
#include "imu_decode.h"
#include "frame_queue.h"
#include "camera_config.h"
```

**Commit:** `refactor: extract imu_sensor.h from video_sensor.h`

---

### Task 8: 拆 `vive_usb.h`

**Files:**
- Create: `vive_usb.h`
- Modify: `vive_tracker.h` (删 unbind/rebind 函数, 加 `#include "vive_usb.h"`)

从 `vive_tracker.h:18-105` 搬出 `unbind_usbfs_for_vive()` 和 `rebind_usbfs()`。

**Commit:** `refactor: extract vive_usb.h from vive_tracker.h`

---

### Task 9: 更新 Makefile + CMakeLists.txt

**Files:**
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

Makefile 的 `main.o` 规则加新头文件依赖:
```makefile
main.o: main.cpp sensor.h camera_config.h barrier.h \
        video_sensor.h imu_sensor.h encoder_sensor.h vive_tracker.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ main.cpp
```

CMakeLists.txt 无需改（没有新 .cpp）。

**验证:** RK3588 上 `make clean && make`，无编译错误。

**Commit:** `build: update Makefile for split structure`

---

### Task 10: 整体验证

RK3588 上:
```bash
cd unified_capture
make clean && make
./unified_capture --scan
```

确认 4 路摄像头全部识别，无 include 错误。

最后 squash 或保留独立 commit。

---

## 拆分前后对比

| 文件 | 拆分前 | 拆分后 |
|------|--------|--------|
| `sensor.h` | 173 行 (Sensor + FrameQueue + BGRFrame + SimpleBarrier + 工具) | ~40 行 (仅 Sensor 基类) |
| `video_sensor.h` | 542 行 (CameraConfig + MppEncoder + bgr2nv12 + VideoSensor + ImuSensor) | ~200 行 (仅 VideoSensor) |
| `vive_tracker.h` | 228 行 (USB 管理 + Tracker Sensor) | ~120 行 (仅 ViveTrackerSensor) |
| 新增文件 | 0 | 8 个 (`camera_config`, `time_utils`, `barrier`, `frame_queue`, `bgr2nv12`, `mpp_encoder`, `imu_sensor`, `vive_usb`) |

## 拆分后文件清单

```
unified_capture/
├── 基础设施 (header-only, 无 SDK 依赖)
│   ├── camera_config.h      ← Task 1
│   ├── time_utils.h         ← Task 2
│   ├── barrier.h            ← Task 3
│   ├── frame_queue.h        ← Task 4
│   └── bgr2nv12.h           ← Task 5
│
├── 硬件设备
│   ├── sensor.h             ← Task 2-4 瘦身
│   ├── video_sensor.h       ← Task 1,4-7 瘦身
│   ├── mpp_encoder.h        ← Task 6
│   ├── imu_sensor.h         ← Task 7 (新建)
│   ├── encoder_sensor.h     ← 不动
│   ├── vive_tracker.h       ← Task 8 瘦身
│   └── vive_usb.h           ← Task 8 (新建)
│
├── 纯 C (不动)
│   ├── imu_decode.h
│   ├── as5600.h / as5600.c
│
├── main.cpp                 ← Task 1,7 更新 include
├── Makefile                 ← Task 9
└── CMakeLists.txt           ← Task 9
```
