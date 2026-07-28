# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

RK3588 四路摄像头统一采集程序。支持 2 路 JHH2 独立双目 + 1 台六目模组（JHH02 双目 + JHH04 四目），H.265 硬件编码 + Y8 原始灰度同步输出。目标平台为 RK3588 ARM64 板端。

SDK: **Nori Xvision v10.00.09** (替代旧 TSTC USBCam_API v1.0.0)

## 编译

```bash
# 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     NORI_INC=/path/to/nori/include NORI_LIB=/path/to/nori/lib

# 扫描设备
make scan

# 测试（无硬件回归）
make test
```

生产程序的编译单元为 `app/*.cpp`、`hardware/common/sensor.cpp`、`hardware/video/device_discovery.cpp` 和 `hardware/as5600/as5600.c`；其余实现位于 header 中并由这些编译单元包含。

## 架构

### 线程模型

Socket 通信全部合并到主线程 poll() 中处理。

```
app/main.cpp
 └── Runtime (app/runtime.cpp，单线程 poll)
      ├── SocketServer         → /tmp/unified_capture.sock
      ├── discover_cameras()   → Nori Xvision 设备枚举
      ├── GpioControl          → GPIO 按键与指示灯
      └── SessionRunner
           ├── VideoSensor × 2   → JHH2 左/右（各自 std::thread）
           ├── SixCamSensor × 1  → JHH02 + JHH04 双通道
           ├── ImuSensor × 最多 4 → JHH2 左、JHH2 右、JHH02、JHH04 各自从 FrameQueue 异步消费 BGR 帧
           ├── EncoderSensor × 1 → AS5600 I2C 读取（100 Hz）
           └── ViveTracker × 1   → VIVE 姿态（检测到设备时）
```

### Sensor 基类 — 生命周期 (`hardware/common/sensor.h`)

每个 Sensor 在新 `std::thread` 中运行三段式生命周期：

1. `setup()` — 设备初始化、打开文件、启动子进程
2. `collect()` — 主采集循环，检查 `running_` 标志
3. `teardown()` — 清理、等待子进程、关闭文件

**同步机制：** 所有 sensor 线程在 `setup()` 完成后通过 `SimpleBarrier` 同步，然后同时进入 `collect()`。`SessionRunner` 通过 `wait_all_arrived()` 轮询 barrier 状态（不加入 barrier），并调用 `Runtime` 提供的控制泵继续处理 Socket 和 GPIO 事件。

### SimpleBarrier (`core/barrier.h`) — C++20 std::barrier 替代

兼容 GCC 10，支持两种使用模式：
- `arrive_and_wait()` — sensor 线程调用，阻塞直到所有线程到达
- `wait_all_arrived(timeout_ms)` — `SessionRunner` 轮询用，不入 barrier，超时返回 false

### 视频采集管道

```
Nori Xvision SDK (MJPEG, GetFrameBuff 轮询)
  → turbojpeg 解码 → BGR24
    ├→ FrameQueue → ImuSensor (异步消费，码带解码)
    ├→ bgr2nv12 → NV12
    │   ├→ MPP H.265 硬件编码 → FIFO → ffmpeg 子进程 → MKV
    │   └→ Y8 原始灰度 (写 NV12 的 Y 平面，即 w×h 字节)
    └→ Preview JPEG (按需，1/4 缩放)
```

**色彩转换注意：** Y8 输出实际写的是 NV12 的 Y 平面（实际 JPEG 解码尺寸），不是独立提取。

### Runtime 与 session 状态

- `Runtime::keep_running_` (atomic bool) — 控制主运行循环是否继续。
- `Runtime::session_running_` (atomic bool) — 当前 session 与全部 sensor 线程共享的停止信号。
- `g_t0` (timespec) — 所有时间戳的 CLOCK_MONOTONIC 纪元，在 `SessionRunner::run()` 开始时设置。
- `SessionRunner::capture_control_` — `VideoCaptureControl` 协调 JHH02、独立 JHH2 与 JHH04 的启流依赖，以及预览导出请求。

### 设备匹配

摄像头由 `discover_cameras()` 调用 Nori Xvision 枚举。JHH04（`1bcf:2d51`）确定 SixCam 所在 USB bus，JHH02（`1bcf:2d50`）在同一 bus 上匹配；其余 `1bcf:2d50` 设备按枚举顺序分配给独立 JHH2 左/右目：
- **JHH2 双目**: `1bcf:2d50`, 3840×1200@30fps
- **JHH02 (六目双目侧)**: `1bcf:2d50`, 4000×1200@30fps
- **JHH04 (六目四目侧)**: `1bcf:2d51`, 3104×480@30fps
- **VIVE Tracker**: `28de:2300`, sysfs 自动检测

### 启流顺序（IMU 硬件依赖，非 SDK 限制）

1. SixCam JHH02 先启流。
2. 独立 JHH2 左/右目等待 JHH02 完成后并行启流。
3. SixCam JHH04 等待 JHH02 与独立 JHH2 完成启流。

`VideoCaptureControl` 管理该硬件依赖；Nori Xvision SDK 支持多路并发启动，无需全局互斥锁。

### 运行模式

| 模式 | 触发方式 | 说明 |
|------|---------|------|
| GPIO | `./unified_capture experiment_001` | 按下按钮开始/停止；输出写入 `/media/usb0/capture/experiment_001` |
| Socket | `--socket` | 监听 `/tmp/unified_capture.sock`，纯文本 JSON 协议 |
| 启动即录 | `--no-gpio` | 启动后立即采集，Ctrl-C 停止 |
| 单次 | `--single` | 完成一次 session 后退出（配合 systemd） |

### Socket 协议

Unix Domain Socket，路径 `/tmp/unified_capture.sock`，每条命令以换行结束：

- `status` → JSON（含 cameras ready/running/elapsed 状态）
- `start` → 开始采集
- `stop` → 停止采集
- `preview:<path>` → 导出当前帧 JPEG 缩略图到指定路径

### 输出结构 (SD 卡)

```
/media/usb0/capture/experiment_001/session_001/
├── jhh2_left/   → jhh2_left-<timestamp>.mkv + .y8 + .jsonl（IMU）
├── jhh2_right/  → jhh2_right-<timestamp>.mkv + .y8 + .jsonl（IMU）
├── jhh02/       → jhh02-<timestamp>.mkv + .y8 + .jsonl（IMU）
├── jhh04/       → jhh04-<timestamp>.y8 + .jsonl（IMU）
├── encoder-<timestamp>.jsonl   → AS5600
├── tracker_raw.jsonl           → VIVE 原始 pose
└── tracker.jsonl               → VIVE 每设备 100 Hz 重采样 pose
```

### 关键依赖（仅 RK3588 板端可用）

- **Nori Xvision SDK v10.00.09** (`Nori_Xvision_API.h`, `libNori_Xvision_Std`) — USB3 Vision 驱动，安装在 `/usr/local/Nori_Xvision/`
- **Rockchip MPP** (`librockchip_mpp`) — H.265 硬件编码
- **libturbojpeg** — MJPEG 解码
- **libgpiod** — GPIO 按键
- **libsurvive** — VIVE Tracker（检测到设备时自动启用）
- **FFmpeg** — MKV 封装（运行时 fork+exec）

## 设计决策记录

`docs/decisions/` 包含关键架构决策：
- IMU 从 BGR 帧异步解码，不阻塞视频管道
- 使用 MKV 容器封装 H.265 视频流
- Socket 通信合入主线程 poll() 而非独立线程
- Preview JPEG 在 sensor collect 循环中按需导出，不创建额外线程
- 2026-07-27 从 TSTC USBCam_API v1.0.0 迁移到 Nori Xvision SDK v10.00.09（解决多 Session 死锁，简化启流模型）

## 注意事项

- 所有输出必须落在 SD 卡 `/media/usb0/capture/` 下，程序启动时强制检查
- AS5600 驱动为纯 C (`hardware/as5600/as5600.c`)，通过 `extern "C"` 在 C++ 中调用
- MPP 编码器需要 NV12 输入，实际 JPEG 解码尺寸可能与配置尺寸不同，代码按首帧动态分配 NV12 buffer
- VIVE Tracker 在 sysfs 中自动检测，不需要手动禁用
