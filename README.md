# unified_capture

RK3588 统一采集程序。`mango` 保持原有 JHH2 独立双目与六目模组；`banana` 采集左、右腕部单目；`cherry` 采集 YCTC SC233HGS 双目模组的 UVC H.264 与 CDC ACM 串口传感器流。

## 硬件

| 设备 | VID/PID | 分辨率 | 帧率 | 输出 |
|------|---------|--------|------|------|
| JHH2 左目 | 1bcf:2d50 | 3840×1200 | 30fps | H.265 MKV + `.y8` + IMU JSONL |
| JHH2 右目 | 1bcf:2d50 | 3840×1200 | 30fps | H.265 MKV + `.y8` + IMU JSONL |
| SixCam JHH02（双目） | 1bcf:2d50 | 4000×1200 | 30fps | H.265 MKV + `.y8` + IMU JSONL |
| SixCam JHH04（四目） | 1bcf:2d51 | 3104×480 | 30fps | `.y8` + IMU JSONL |
| AS5600 编码器 | I2C 0x36 | — | 100Hz | JSONL |
| VIVE Tracker 3.0 | USB HID | — | — | pose JSONL |
| banana 左腕 | Nori `SL`（可配置） | 1440×960 | 30fps | H.265 MKV + IMU JSONL（无 Y8） |
| banana 右腕 | Nori `JHHSW`（可配置） | 1440×960 | 30fps | H.265 MKV + IMU JSONL（无 Y8） |
| cherry 双目 | 5268:1218（同 USB 父设备配对 UVC + ttyACM） | 3200×1200 | 30fps | H.264 MKV + IMU/MAG/FRAME_META/视频帧 JSONL（无 Y8） |

Cherry 腕部相机尚未到货，本版本只保留配置字段，不把腕部硬件描述为可用能力。

## 依赖（RK3588 板端）

- **Nori Xvision SDK** — USB3 Vision 摄像头驱动
- **Rockchip MPP** — H.265 硬件编码
- **libturbojpeg** — MJPEG → BGR 解码
- **libgpiod** — GPIO 按键控制
- **libsurvive** — VIVE Tracker（可选）
- **FFmpeg** — MKV 封装（运行时依赖）

## 编译

```bash
# RK3588 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     NORI_INC=/path/to/nori/include NORI_LIB=/path/to/nori/lib
```

## 运行

### 产品配置

服务启动前必须读取以下两个文件；示例文件可直接作为初始配置安装：

```bash
install -d /etc/unified_capture
cp deploy/product.conf.example /etc/unified_capture/product.conf
cp deploy/camera-map.conf.example /etc/unified_capture/camera-map.conf
```

`product.conf` 可选择 `mango`、`banana` 或 `cherry`。`camera-map.conf` 的 `[cherry]` 段固定要求 `5268:1218`、`3200x1200`、`H264`、30 fps；发现逻辑通过同一 sysfs USB device 父路径配对 `/dev/videoN` 与 `/dev/ttyACM*`，不会仅凭 bus number 配对。初始 banana 映射为左腕 `SL`、右腕 `JHHSW`，可按硬件修改。banana 的 `allow_missing_devices=true` 表示缺失一侧或两侧时服务仍会就绪，`status` 返回 `"degraded":true`，并允许 `start`/`stop`。配置错误仍会阻止服务启动。

banana 固定输出 H.265 MKV 与从视频帧异步解码的 IMU JSONL，不生成 `.y8`，也不启动 AS5600 或 VIVE。腕部是否暴露麦克风及其 USB 音频身份、声道、采样率和左右关联仍是待确认事项；本版本不枚举、录制或封装音频。

cherry 将相机已编码的 H.264 access unit 直接交给 FFmpeg remux 为 MKV，不做 JPEG 解码或 MPP 二次编码；串口独立输出 IMU、MAG 与 FRAME_META。FRAME_META 依赖板端 GPIO 帧同步脉冲接线，未接线时 `frame_meta.jsonl` 可以是合法空文件，但 MKV、IMU 和 MAG 仍是必需产物。

### GPIO 按键模式（调试/手动）

```bash
./unified_capture experiment_001
```

按下 GPIO 按键开始录像，再按停止。支持 socket 命令并发控制。

### Socket 模式（systemd 部署）

```bash
./unified_capture --socket experiment_001
```

启动后监听 `/tmp/unified_capture.sock`，等待 socket 命令控制启停。

### 命令行参数

```
--scan          扫描 Nori 设备并退出
--no-gpio       不使用 GPIO，启动后立即采集
--no-imu        禁用 IMU 采集
--no-as5600     禁用 AS5600 编码器
--no-h265       只输出 .y8 原始灰度，不生成 MKV
--socket        纯 socket 模式（无 GPIO），适合 systemd
--single        完成一个 session 后退出
--config <path> 使用指定 product.conf（camera-map 仍为 /etc/unified_capture/camera-map.conf）
-h, --help      显示用法
output_prefix   相对路径写入 /media/usb0/capture/；绝对路径必须在该目录下
```

## Socket 控制协议

Unix Domain Socket，路径 `/tmp/unified_capture.sock`，纯文本协议，每条命令以换行结束。
完整字段、状态语义与前端接入示例见 [Socket 控制协议](docs/socket-control.md)。

```bash
# 状态查询
echo "status" | nc -U /tmp/unified_capture.sock
# → {"ok":true,"ready":true,"running":false,"cameras":{"jhh2_left":true,...}}

# banana: {"product":"banana","degraded":false,"cameras":{"wrist_left":true,"wrist_right":true},...}

# 开始采集
echo "start" | nc -U /tmp/unified_capture.sock
# → {"ok":true}

# 停止采集
echo "stop" | nc -U /tmp/unified_capture.sock
# → {"ok":true,"elapsed_ms":8032}
```

## systemd 部署

```bash
cp unified_capture /usr/local/bin/
cp deploy/unified_capture.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable unified_capture
systemctl start unified_capture
```

查看日志：`journalctl -u unified_capture -f`

## 输出数据结构

banana 的每次 session 仅创建已发现的 `wrist_left/`、`wrist_right/` 目录；每个目录包含 `*.mkv`（HEVC）与 `*.jsonl`（异步 IMU），不包含 `.y8`。板端验收见 [tests/README.md](tests/README.md)。

cherry 的每次 session 创建 `cherry_stereo/`，其中包含 `cherry_stereo.mkv`（H.264 3200×1200@30）、`video_frames.jsonl`、`imu.jsonl`、`mag.jsonl` 与 `frame_meta.jsonl`，不包含 `.y8`。同步分析使用 `python3 deploy/calc_cherry_sync.py <session_dir>`。

```
/media/usb0/capture/experiment_001/
└── session_001/
    ├── jhh2_left/
    │   ├── jhh2_left-<timestamp>.mkv    # H.265 → MKV
    │   ├── jhh2_left-<timestamp>.y8     # 原始灰度
    │   └── jhh2_left-<timestamp>.jsonl  # IMU
    ├── jhh2_right/
    │   ├── jhh2_right-<timestamp>.mkv
    │   ├── jhh2_right-<timestamp>.y8
    │   └── jhh2_right-<timestamp>.jsonl # IMU
    ├── jhh02/
    │   ├── jhh02-<timestamp>.mkv
    │   ├── jhh02-<timestamp>.y8
    │   └── jhh02-<timestamp>.jsonl      # IMU
    ├── jhh04/
    │   ├── jhh04-<timestamp>.y8
    │   └── jhh04-<timestamp>.jsonl      # IMU
    ├── encoder-<timestamp>.jsonl         # AS5600
    ├── tracker_raw.jsonl                  # VIVE 原始 pose
    └── tracker.jsonl                      # VIVE 每设备 100 Hz 重采样 pose
```

## 架构

```
app/main.cpp
 └── Runtime (app/runtime.cpp，单线程 poll)
      ├── SocketServer       → /tmp/unified_capture.sock
      ├── discover_cameras() → Nori 设备枚举
      ├── GpioControl
      └── SessionRunner
           ├── VideoSensor × 2   → JHH2 左/右（各自线程）
           ├── SixCamSensor × 1  → JHH02 + JHH04（双通道）
           ├── ImuSensor × 4     → IMU 码带解码
           ├── EncoderSensor × 1 → AS5600
           └── ViveTracker × 1   → VIVE 姿态（检测到设备时）
```

cherry session 则创建 `CherryVideoSensor × 1` 和 `CherrySerialSensor × 1`；串口先完成 START，再由二者通过 barrier 同步进入采集。

Socket 通信合并到 `Runtime` 主线程的 `poll()`，不创建独立 Socket 线程。

## 启流顺序

1. SixCam JHH02（六目双目侧）
2. JHH2 左目和右目（独立双目，并行）
3. SixCam JHH04（六目四目侧，等待 JHH02 与独立 JHH2 启流）

`VideoCaptureControl` 协调以上 IMU 硬件依赖顺序；Nori Xvision SDK 支持多路并发启动。

## 源码布局

| 目录 | 职责 |
|------|------|
| `app/` | 程序入口、运行时控制、Socket/GPIO 控制与 session 生命周期 |
| `core/` | 与硬件无关的配置、同步、输出路径和时间工具 |
| `hardware/` | 摄像头、IMU、AS5600、VIVE Tracker 与 Cherry UVC/串口设备实现 |
| `tests/` | 主机可运行的单元/布局测试与板端 Socket 验收脚本 |
| `deploy/` | systemd service、RK3588 同步脚本与离线同步分析工具 |
