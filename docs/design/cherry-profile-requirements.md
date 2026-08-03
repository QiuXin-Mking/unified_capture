# Cherry Profile 需求规格

> 状态: **需求已确认 (Grill 完毕)**
> 日期: 2026-08-03
> Profile 代号: **cherry**

## 1. 概述

新增 `cherry` profile，基于郭总 YCTC SC233HGS 双目摄像头模组。与现有 mango/banana 的关键差异：

| 维度 | mango | banana | **cherry** |
|------|-------|--------|------------|
| 主相机 | JHH2 独立双目 (Nori) | 腕部单目 (V4L2 MJPEG) | **YCTC SC233HGS 双目 (UVC H.264)** |
| 视频协议 | Nori Xvision SDK | V4L2 MJPEG | **V4L2 H.264 (UVC)** |
| IMU 来源 | BGR 码带解码 | BGR 码带解码 | **CDC ACM 串口协议** |
| MAG 来源 | 无 | 无 | **CDC ACM 串口协议** |
| 腕部相机 | 无 | V4L2 MJPEG | **YCTC SC233HGS 同平台 (预留)** |
| AS5600 | 有 | 无 | **无** |
| VIVE | 有 | 无 | **无** |
| Y8 输出 | 有 | 无 | **无** |

## 2. 硬件规格

### 2.1 双目模组 (郭总)

| 项目 | 值 |
|------|-----|
| 产品名 | YCTC_SC233HGS |
| 芯片平台 | Hi3516CV610 |
| USB VID/PID | `0x5268` / `0x1218` |
| USB 速率 | USB 2.0 High-Speed (480Mbps) |
| USB 接口枚举 | Interface 0/1: UVC Video; Interface 2: CDC ACM |
| 物理连接 | **一块板，一条 USB 线** |

### 2.2 UVC 视频格式

> 2026-08-03 在 `root@192.168.100.200` 上用 `v4l2-ctl --list-formats-ext`
> 实测确认：当前固件只枚举 H.264、HEVC 和 MJPEG，不枚举 NV12。
> 用户已确认改为直接采集 UVC H.264 并 remux 到 MKV。

| 分辨率 | 像素格式 | FourCC | 帧率 | 选用 |
|--------|----------|--------|------|------|
| 3200×1200 | H.264 | `H264` | 30fps | **✓ 选用** |
| 3200×1200 | H.265/HEVC | `HEVC` | 30fps | |
| 3200×1200 | Motion-JPEG | `MJPG` | 30fps | |
| 3840×1080 | H.265/H.264/MJPEG | `HEVC`/`H264`/`MJPG` | 30fps | |

- 双目拼接：**左 1600×1200 + 右 1600×1200** = 3200×1200
- 当前固件输出已压缩视频，Cherry 选用 H.264，不做 JPEG 解码或 MPP 二次编码
- 固定 30fps

### 2.3 串口协议 (Sensor Bridge v3)

| 参数 | 值 |
|------|-----|
| 接口 | USB CDC ACM |
| Linux 设备节点 | `/dev/ttyACM*` |
| 波特率 | 921600 8N1 |
| 字节序 | Little-endian |
| 协议版本 | `0x03` |

数据流通道（通过 `START` 命令 `stream_mask` 选择）：

| Bit | 值 | 数据流 | cherry 需要 |
|-----|-----|--------|------------|
| 0 | `0x01` | IMU (陀螺仪+加速度计, ~500Hz) | ✓ |
| 1 | `0x02` | MAG (磁力计, ~100Hz) | ✓ |
| 2 | `0x04` | FRAME_META (GPIO 帧同步元数据) | ✓ |

**完整协议规范**: `YCTC_SC233HGS_protocol/docs/UART_PROTOCOL.md`

**关键特性**:
- IMU/MAG 输出不依赖视频流是否打开
- FRAME_META 只在媒体管线输出帧时产生，由 GPIO 同步脉冲事件生成
- IMU 和 MAG 只有**一路**（板级共享，不分左右目）

**IMU 物理量换算**:
- 加速度计: `4g / 32768 = 0.0001220703125 g/LSB`
- 陀螺仪: `1000 dps / 32768 = 0.030517578125 dps/LSB`

### 2.4 腕部相机 (预留)

| 项目 | 值 |
|------|-----|
| 状态 | **未到货，占位** |
| 硬件平台 | 同 YCTC SC233HGS (预期) |
| 接口 | UVC + CDC ACM 串口（实际格式待到货确认） |
| 数量 | 2 个 (wrist_left / wrist_right) |
| 每设备 USB 口 | 1 个 (独立 UVC + CDC ACM) |
| 分辨率 | // TODO: 需要摄像头到货后确认 |
| 帧率 | // TODO: 需要摄像头到货后确认 |

## 3. 软件架构

### 3.1 设计原则

1. **新增不改旧**: 所有新功能通过继承 Sensor 基类实现，不修改已有 VideoSensor / ImuSensor / SixCamSensor
2. **独立解耦**: 视频采集和串口数据采集各自独立 Sensor，生命周期独立
3. **可复用**: CherrySerialSensor 参数化设计，双目板和腕部相机复用同一个类

### 3.2 新增类

#### CherryVideoSensor (继承 Sensor)

```
V4L2 H.264 → FIFO → ffmpeg remux → MKV
```

- 通过 V4L2 直接抓取 H.264 access unit（无需 JPEG 解码，无需色彩转换）
- 复用现有 `V4l2Device` 类
- 复用现有 capture queue 和 ffmpeg FIFO/MKV 封装方式，不经过 MPP
- 不复用 `VideoSensor`（后者基于 MJPEG 路径）
- 输出: `cherry_stereo.mkv`（完整 3200×1200 拼接画面，不裁切）
- 每帧写 `video_frames.jsonl`，记录 V4L2 sequence + CLOCK_MONOTONIC timestamp

#### CherrySerialSensor (继承 Sensor)

```
CDC ACM open → START(stream_mask=0x07) → recv loop (IMU_DATA/MAG_DATA/FRAME_META) → STOP → close
```

- 打开 `/dev/ttyACM*`，921600 8N1
- 解析 YCTC Sensor Bridge v3 协议帧（magic 校验 + CRC16 校验）
- 输出 3 个 JSONL:
  - `imu.jsonl` — IMU 采样数据 (gyro+acc, 含 pts_us)
  - `mag.jsonl` — MAG 采样数据 (含 pts_us)
  - `frame_meta.jsonl` — FRAME_META (含 frame_pts_us, sensor_idx)
- 参数化构造：传入串口设备路径 + sensor 名称

### 3.3 设备发现

- 通过 **USB device sysfs 父路径** 关联同一块板的 UVC (`/dev/videoN`) 和 CDC ACM (`/dev/ttyACMx`)
- 扫描 `/sys/class/video4linux/` + `/sys/class/tty/`，匹配同一 USB device 父节点且 VID/PID 为 `0x5268:0x1218` 的节点
- `busnum` 仅用于日志；同一 root hub 下的多个设备可共享 bus number，不能作为唯一配对键
- 腕部相机同理，按各自的 USB device 父路径独立匹配

### 3.4 启流顺序

cherry 双目板只有一个 VideoSensor + 一个 SerialSensor，无硬件依赖：

1. CherrySerialSensor 先发送 START，等待响应
2. CherryVideoSensor 启动 V4L2 流
3. FRAME_META 在视频流启动后自动产生

带腕部相机时的顺序（硬件到货后）:

1. 双目 CherrySerialSensor START
2. 腕部 CherrySerialSensor × 2 START
3. 所有 CherryVideoSensor 同步启流（通过 SimpleBarrier）

### 3.5 Profile 配置

`/etc/unified_capture/product.conf`:
```ini
product=cherry
```

`/etc/unified_capture/camera-map.conf`:
```ini
[cherry]
# 双目模组设备匹配
stereo.vid=0x5268
stereo.pid=0x1218
stereo.resolution=3200x1200
stereo.format=H264
stereo.fps=30

# 腕部相机 (预留)
allow_missing_devices=true
wrist_left.product=  # TODO: 需要摄像头到货后确认 iProduct
wrist_right.product= # TODO: 需要摄像头到货后确认 iProduct
```

### 3.6 输出目录结构

```
session_001/
├── cherry_stereo/
│   ├── cherry_stereo.mkv       # H.264 完整拼接 3200×1200
│   ├── imu.jsonl               # IMU 数据 (陀螺仪+加速度计, pts_us)
│   ├── mag.jsonl               # MAG 数据 (磁力计, pts_us)
│   ├── frame_meta.jsonl        # FRAME_META (frame_pts_us, sensor_idx)
│   └── video_frames.jsonl      # 视频帧 V4L2 时间戳
├── wrist_left/                 # 预留: 腕部左
│   └── ...
└── wrist_right/                # 预留: 腕部右
    └── ...
```

### 3.7 JSONL 数据格式

#### imu.jsonl

每条记录为一个协议帧的完整 IMU 批次:

```json
{
  "generation": 1,
  "window_begin_pts_us": 123456789,
  "window_end_pts_us": 123466789,
  "gyro_samples": [
    {"x": -12, "y": 34, "z": 56, "temperature": 25000, "pts_us": 123456789},
    ...
  ],
  "acc_samples": [
    {"x": 2360, "y": -450, "z": 7830, "temperature": 25000, "pts_us": 123456789},
    ...
  ]
}
```

#### mag.jsonl

```json
{
  "generation": 1,
  "samples": [
    {"x_raw": 521700, "y_raw": 527700, "z_raw": 523800, "tout_raw": 255, "pts_us": 123456789},
    ...
  ]
}
```

#### frame_meta.jsonl

```json
{
  "generation": 1,
  "samples": [
    {"sensor_idx": 0, "vi_pipe": 0, "frame_id": 100, "frame_pts_us": 123456789},
    {"sensor_idx": 1, "vi_pipe": 0, "frame_id": 100, "frame_pts_us": 123456789}
  ]
}
```

#### video_frames.jsonl

```json
{
  "v4l2_sequence": 0,
  "v4l2_timestamp_us": 123456789
}
```

## 4. 同步验证

### 4.1 对比维度

| 对比对 | 数据源 A | 数据源 B | 匹配键 |
|--------|----------|----------|--------|
| IMU ↔ 视频 | `imu.jsonl` 各 sample 的 `pts_us` | `frame_meta.jsonl` 的 `frame_pts_us` | 时间邻近最近邻 |
| IMU ↔ MAG | `imu.jsonl` 各 sample 的 `pts_us` | `mag.jsonl` 各 sample 的 `pts_us` | 时间邻近最近邻 |
| MAG ↔ 视频 | `mag.jsonl` 各 sample 的 `pts_us` | `frame_meta.jsonl` 的 `frame_pts_us` | 时间邻近最近邻 |

### 4.2 分析方法

参考 `deploy/calc_sync_exp_end.py` 方法论:

- 对每个 A 帧/sample，找时间最近的 B 帧/sample
- 计算时间差绝对值，忽略 >100ms 的（跨帧周期）
- 输出: p50、p90、≤10us 比例、≤100us 比例、min、max

### 4.3 验证脚本

新增 `deploy/calc_cherry_sync.py`，用法:

```bash
python3 calc_cherry_sync.py <session_dir>
python3 calc_cherry_sync.py user@host:<session_path>
```

## 5. 今日可达目标 (2026-08-03)

### 5.1 可完成

| # | 目标 | 依赖 |
|---|------|------|
| 1 | MKV H.264 数据 | V4L2 H.264 抓取 + ffmpeg remux |
| 2 | IMU 数据 | CDC ACM 串口 START/IMU_DATA 解析 |
| 3 | MAG 数据 | CDC ACM 串口 MAG_DATA 解析 |

### 5.2 待验证/依赖条件

| 项目 | 阻碍 | 验证方式 |
|------|------|----------|
| 串口是否出两路 IMU/MAG | 不确定 | `ssh root@192.168.100.200` 验证 |
| FRAME_META | 依赖 GPIO 同步脉冲，板端是否有接线未确认 | 需硬件确认 |
| 同步验证 demo | FRAME_META 数据是 IMU↔视频 和 MAG↔视频 对比的前提 | 无 FRAME_META 时可先跑 IMU↔MAG 对比 |
| 腕部相机 | 硬件未到货 | 仅预留占位 |

## 6. 编译与配置变更

### 6.1 编译单元新增

```
hardware/cherry/
├── cherry_video_sensor.h       # CherryVideoSensor
├── cherry_video_sensor.cpp
├── cherry_serial_sensor.h      # CherrySerialSensor
├── cherry_serial_sensor.cpp
├── cherry_protocol.h           # YCTC Sensor Bridge v3 协议编解码
└── cherry_protocol.cpp
```

协议编解码参考郭总 SDK: `YCTC_SC233HGS_protocol/src/serial_protocol/yctc_bridge_protocol.c`

### 6.2 配置变更

- `core/product_config.h`: 新增 `ProductProfile::cherry`
- `core/product_config.cpp`: `product=cherry` 解析
- `hardware/video/device_discovery.cpp`: 新增 `discover_cherry_cameras()`
- `app/session_runner.cpp`: 新增 cherry 分支（创建 CherryVideoSensor + CherrySerialSensor）
- `app/runtime.cpp`: cherry 的 H.264 强制 + AS5600/VIVE 禁用逻辑

### 6.3 测试

- `tests/test_cherry_protocol.cpp`: 协议编解码单元测试
- `tests/test_cherry_product_config.cpp`: cherry 配置解析测试
