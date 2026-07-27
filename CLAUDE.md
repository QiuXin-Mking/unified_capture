# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

RK3588 四路摄像头统一采集程序。支持 2 路 JHH2 独立双目 + 1 台六目模组（JHH02 双目 + JHH04 四目），H.265 硬件编码 + Y8 原始灰度同步输出。目标平台为 RK3588 ARM64 板端。

## 编译

```bash
# 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     TSTC_INC=/path/to/tstc/include TSTC_LIB=/path/to/tstc/lib

# 扫描设备
make scan

# 测试
make test_output_path    # 构建 + 运行 output_path 测试
make test_time_utils     # 构建 + 运行 time_utils 测试
make test_hardware_header_layout  # 运行 shell 测试
```

源码只有两个编译单元：`main.cpp` (C++20) 和 `hardware/as5600/as5600.c` (C11)。所有其他 `.h` 文件均为 header-only，直接 include 使用。

## 架构

### 线程模型

**关键约束：禁止 pthread_create 用于 socket 通信。** TSTC SDK + MPP 对额外线程敏感，额外的 `pthread_create` 会污染驱动状态导致 MPP 崩溃。Socket 通信全部合并到主线程 poll() 中处理。

```
main (单线程 poll)
 ├── socket_setup()           → /tmp/unified_capture.sock (非阻塞)
 ├── resolve_camera_devices() → 匹配 USB VID/PID，分配设备指针
 ├── poll(gpio_fd + sock_fd)  → 主循环事件分发
 └── run_session()
      ├── VideoSensor × 2     → JHH2 左/右 (各自 std::thread)
      ├── SixCamSensor × 1    → JHH02 + JHH04 (同一个 thread)
      ├── ImuSensor × 3       → 从 FrameQueue 异步消费 BGR 帧
      ├── EncoderSensor × 1   → AS5600 I2C 读取 (100Hz)
      └── ViveTracker × 1     → VIVE 姿态 (libsurvive)
```

### Sensor 基类 — 生命周期 (sensor.h)

每个 Sensor 在新 `std::thread` 中运行三段式生命周期：

1. `setup()` — 设备初始化、打开文件、启动子进程
2. `collect()` — 主采集循环，检查 `running_` 标志
3. `teardown()` — 清理、等待子进程、关闭文件

**同步机制：** 所有 sensor 线程在 `setup()` 完成后通过 `SimpleBarrier` 同步，然后同时进入 `collect()`。`main` 线程通过 `wait_all_arrived()` 轮询 barrier 状态（不加入 barrier），期间仍可处理 socket 命令。

### SimpleBarrier (barrier.h) — C++20 std::barrier 替代

兼容 GCC 10，支持两种使用模式：
- `arrive_and_wait()` — sensor 线程调用，阻塞直到所有线程到达
- `wait_all_arrived(timeout_ms)` — main 线程轮询用，不入 barrier，超时返回 false

### 视频采集管道

```
TSTC SDK (MJPEG)
  → turbojpeg 解码 → BGR24
    ├→ FrameQueue → ImuSensor (异步消费，码带解码)
    ├→ bgr2nv12 → NV12
    │   ├→ MPP H.265 硬件编码 → FIFO → ffmpeg 子进程 → MKV
    │   └→ Y8 原始灰度 (写 NV12 的 Y 平面，即 w×h 字节)
    └→ Preview JPEG (按需，1/4 缩放)
```

**色彩转换注意：** Y8 输出实际写的是 NV12 的 Y 平面（实际 JPEG 解码尺寸），不是独立提取。

### 关键全局状态

- `g_t0` (timespec) — 所有时间戳的 CLOCK_MONOTONIC 纪元，在 `run_session()` 开始时设置
- `g_session_running` (atomic bool) — 主循环和所有 sensor 线程的停止信号
- `g_stream_start_mutex` — 全局互斥锁，串行化 TSTC SDK 的 DEAL_WITH_INIT / STREAM_STATUS 调用（同 VID/PID 设备并发会导致死锁）
- `g_jhh2_remaining` (atomic int) — SixCam 的 jhh04 通道等待 jhh02 和 独立 JHH2 共 3 路启流完成后才开始流
- `g_jhh02_init_done` (atomic bool) — 独立 JHH2 等待六目 jhh02 先完成启流

### 设备匹配

摄像头通过 USB VID/PID + 组内序号 (`group_order`) 匹配：
- **JHH2 双目**: `1bcf:2d50`, 3840×1200@30fps, group_order 0/1
- **JHH02 (六目双目侧)**: `1bcf:2d50`, group_order 2
- **JHH04 (六目四目侧)**: `1bcf:2d51`, 3104×480@30fps
- **VIVE Tracker**: `28de:2300`, sysfs 自动检测

### 启流顺序（严格，不可改变）

1. SixCam JHH02 先启流（`g_jhh02_init_done = true`）
2. 独立 JHH2 左目等待 jhh02 完成后再启流
3. 独立 JHH2 右目同上
4. SixCam JHH04 等待 `g_jhh2_remaining` 归零后启流

所有同 VID/PID 设备的 `DEAL_WITH_INIT → stream_thread → STREAM_STATUS` 在 `g_stream_start_mutex` 保护下完整串行。

### 运行模式

| 模式 | 触发方式 | 说明 |
|------|---------|------|
| GPIO | `./unified_capture /data/capture` | 按下按钮开始/停止 |
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
/media/usb0/capture/session_001/
├── jhh2_left/   → 001.mkv + 001.y8
├── jhh2_right/  → 001.mkv + 001.y8
├── jhh02/       → 001.mkv + 001.y8 + imu.jsonl
├── jhh04/       → 001.y8 + imu.jsonl
└── as5600.jsonl / tracker_raw.jsonl
```

### 关键依赖（仅 RK3588 板端可用）

- **TSTC SDK** (`USBCam_API.h`, `libUSBCam_API`) — USB3 Vision 驱动
- **Rockchip MPP** (`librockchip_mpp`) — H.265 硬件编码
- **libturbojpeg** — MJPEG 解码
- **libgpiod** — GPIO 按键
- **libsurvive** — VIVE Tracker（可选，`--no-vive` 跳过）
- **FFmpeg** — MKV 封装（运行时 fork+exec）

## 设计决策记录

`docs/decisions/` 包含关键架构决策：
- IMU 从 BGR 帧异步解码，不阻塞视频管道
- 使用 MKV 容器封装 H.265 视频流
- Socket 通信合入主线程 poll() 而非独立线程
- Preview JPEG 在 sensor collect 循环中按需导出，不创建额外线程
- 多 session 进程隔离（`--single` + systemd Restart）

## 注意事项

- 所有输出必须落在 SD 卡 `/media/usb0/capture/` 下，程序启动时强制检查
- AS5600 驱动为纯 C (`hardware/as5600/as5600.c`)，通过 `extern "C"` 在 C++ 中调用
- MPP 编码器需要 NV12 输入，实际 JPEG 解码尺寸可能与配置尺寸不同，代码按首帧动态分配 NV12 buffer
- VIVE Tracker 在 sysfs 中自动检测，不需要 `--no-vive` 手动禁用
