# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

RK3588 统一采集程序。`mango` 采集六目模组（JHH02/JHH04）与左、右腕部单目，输出 H.265 MKV 和异步 IMU JSONL；`banana` 保持 2 路 JHH2 独立双目与六目模组（含 AS5600/VIVE）；`cherry` 为 YCTC SC233HGS 双目，直通 UVC H.264 并采集 CDC ACM IMU/MAG/FRAME_META。mango 与 cherry 均不输出 Y8。目标平台为 RK3588 ARM64 板端。

采集方式: **UVC (V4L2)** — 摄像头通过 USB Video Class 协议经 V4L2 从 `/dev/video*` 获取 MJPG/H.264 帧

> **命名已对齐**：前端 UI 的「Mango」产品 = 头部 Ego + 左腕 + 右腕，对应**守护进程的 `mango` profile**（六目 jhh02/jhh04 + 腕部 SL/JHHSW）。守护进程的 `banana` 是 legacy 头部方案（2 路 JHH2 独立双目 + 六目 + AS5600 + VIVE）。

## 编译

```bash
# 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc

# 扫描设备
make scan

# 测试（无硬件回归）
make test
```

生产程序的编译单元包括 `app/*.cpp`、`core/product_config.cpp`、`hardware/common/sensor.cpp`、`hardware/video/device_discovery.cpp`、`hardware/wrist/*.cpp`、`hardware/cherry/*.cpp` 和 `hardware/as5600/as5600.c`。

`/etc/unified_capture/product.conf` 选择 `mango`、`banana` 或 `cherry`；`/etc/unified_capture/camera-map.conf` 保存 UVC `iProduct` 匹配和 Cherry 的严格视频参数。Cherry 通过 sysfs USB device 父路径配对 UVC 与 ttyACM，不能只按 bus number 配对。腕部麦克风和 Cherry 腕部相机能力尚未确认；不要在未另行批准前增加音频或声称腕部硬件可用。

## 架构

### 线程模型

Socket 通信全部合并到主线程 poll() 中处理。

```
app/main.cpp
 └── Runtime (app/runtime.cpp，单线程 poll)
      ├── SocketServer         → /tmp/unified_capture.sock
      ├── discover_cameras()   → V4L2 设备枚举
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
V4L2 (MJPEG)
  → turbojpeg 解码 → BGR24
    ├→ FrameQueue → ImuSensor (异步消费，码带解码)
    ├→ bgr2nv12 → NV12
    │   ├→ MPP H.265 硬件编码 → FIFO → ffmpeg 子进程 → MKV
    │   └→ Y8 原始灰度 (写 NV12 的 Y 平面，即 w×h 字节)
    └→ Preview JPEG (按需，1/4 缩放)
```

**色彩转换注意：** Y8 输出实际写的是 NV12 的 Y 平面（实际 JPEG 解码尺寸），不是独立提取。

Cherry 不走上述 MJPEG/MPP 管线：`V4L2 H264 → FIFO → ffmpeg stream-copy remux → cherry_stereo.mkv`。`CherrySerialSensor` 先发送 START(0x07)，随后并行写入 `imu.jsonl`、`mag.jsonl` 与 `frame_meta.jsonl`。FRAME_META 依赖硬件 GPIO 同步脉冲，接线缺失时文件可能为空；不得因此放宽 MKV、IMU 或 MAG 验收。

### Runtime 与 session 状态

- `Runtime::keep_running_` (atomic bool) — 控制主运行循环是否继续。
- `Runtime::session_running_` (atomic bool) — 当前 session 与全部 sensor 线程共享的停止信号。
- `g_t0` (timespec) — 所有时间戳的 CLOCK_MONOTONIC 纪元，在 `SessionRunner::run()` 开始时设置。
- `SessionRunner::capture_control_` — `VideoCaptureControl` 协调 JHH02、独立 JHH2 与 JHH04 的启流依赖，以及预览导出请求。

### 设备匹配

摄像头由 `discover_cameras()` 调用 V4L2 枚举。JHH04（`1bcf:2d51`）确定 SixCam 所在 USB bus，JHH02（`1bcf:2d50`）在同一 bus 上匹配；其余 `1bcf:2d50` 设备按枚举顺序分配给独立 JHH2 左/右目：
- **JHH2 双目**: `1bcf:2d50`, 3840×1200@30fps
- **JHH02 (六目双目侧)**: `1bcf:2d50`, 4000×1200@30fps
- **JHH04 (六目四目侧)**: `1bcf:2d51`, 3104×480@30fps
- **VIVE Tracker**: `28de:2300`, sysfs 自动检测

### 启流顺序（IMU 硬件依赖，非 SDK 限制）

1. SixCam JHH02 先启流。
2. 独立 JHH2 左/右目等待 JHH02 完成后并行启流。
3. SixCam JHH04 等待 JHH02 与独立 JHH2 完成启流。

`VideoCaptureControl` 管理该硬件依赖；V4L2 支持多路并发启动，无需全局互斥锁。

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
- `preview:<channel>:<path>` → 导出当前帧 JPEG 缩略图到指定路径（channel ∈ `jhh02`/`jhh04`/`wrist_left`/`wrist_right`，也支持无 channel 的 legacy `preview:<path>`）；仅在 `running` 时可用——预览本质是「临时采集 session + 抽帧」，无原生「仅预览不落盘」模式

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

Cherry 输出位于 `session_001/cherry_stereo/`：`cherry_stereo.mkv`、`video_frames.jsonl`、`imu.jsonl`、`mag.jsonl`、`frame_meta.jsonl`，且没有 Y8。可用 `deploy/calc_cherry_sync.py` 对本地或 `user@host:path` session 做同步统计。

### 关键依赖（仅 RK3588 板端可用）

- **Linux UVC 驱动 (V4L2)** — 摄像头经内核 UVC 驱动通过 V4L2 接口获取，无需外部 SDK
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
- 采集接入迁移：TSTC USBCam_API v1.0.0 → Nori Xvision SDK v10.00.09（2026-07-27，解决多 Session 死锁）→ UVC/V4L2（当前，经内核 UVC 驱动获取帧，无外部 SDK）

## 注意事项

- 所有输出必须落在 SD 卡 `/media/usb0/capture/` 下，程序启动时强制检查
- AS5600 驱动为纯 C (`hardware/as5600/as5600.c`)，通过 `extern "C"` 在 C++ 中调用
- MPP 编码器需要 NV12 输入，实际 JPEG 解码尺寸可能与配置尺寸不同，代码按首帧动态分配 NV12 buffer
- VIVE Tracker 在 sysfs 中自动检测，不需要手动禁用

## Git Commit 规范

严格遵循 `.claude/COMMIT_CONVENTIONS.md`，中文描述 + 详细说明改动与收益。违规将被 PreToolUse hook 拦截。
