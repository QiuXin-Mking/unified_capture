# unified_capture 介绍

RK3588 上的**统一多相机采集程序**。一套二进制，通过产品配置文件切换三套硬件方案（`mango` / `banana` / `cherry`），负责相机枚举、视频采集、IMU 解码、传感器读取与数据落盘，并对外提供 socket 控制接口供前端/上位机调用。

---

## 1. 三套产品方案

| profile | 硬件 | 采集内容 | 主要输出 |
|---------|------|----------|----------|
| `mango` | 六目模组(JHH02/JHH04) + 左/右腕部单目（SL/JHHSW） | 4 路视频 + 4 路 IMU 码带 | H.265 MKV + IMU JSONL（无 Y8、无 AS5600） |
| `banana` | 2× JHH2 独立双目 + 六目 + AS5600 + VIVE Tracker | 视频 + IMU 码带 + 角度 + 位姿 | H.265 MKV + IMU/编码器/姿态 JSONL |
| `cherry` | YCTC SC233HGS 双目（UVC H.264 + CDC ACM 串口） | 双目视频 + IMU/MAG/FRAME_META | H.264 MKV + 多个 JSONL（无 Y8） |

> **命名已对齐**：**mango = 六目 + 左腕 + 右腕**（无 AS5600/VIVE），**banana = legacy 头部**（2× JHH2 独立双目 + 六目 + AS5600 + VIVE）。

各方案的详细说明：

- **mango**（六目 + 腕部）：设备组成、采集管线、最终数据包详见 [mango-device-overview.md](mango-device-overview.md)。四路相机均输出 H.265 MKV + 异步 IMU JSONL，不生成 Y8、不启动 AS5600；`allow_missing_devices=true` 时缺失一侧腕部仍可启动（`status` 返回 `degraded:true`）。
- **banana**（legacy 头部）：2× JHH2 独立双目 + 六目 + AS5600 + VIVE，输出 H.265 MKV + IMU/编码器/姿态 JSONL。
- **cherry**：相机已编码的 H.264 直通 ffmpeg remux 成 MKV，不 JPEG 解码、不 MPP 二次编码；串口独立输出 IMU/MAG/FRAME_META。

---

## 2. 架构

```
app/main.cpp
 └── Runtime（单线程 poll，Socket 也合入主线程）
      ├── SocketServer          → /tmp/unified_capture.sock
      ├── discover_cameras()    → V4L2 设备枚举
      ├── GpioControl           → GPIO 按键/指示灯
      └── SessionRunner         → 每 session 一组 sensor
           ├── VideoSensor × N  → 独立 JHH2 / 腕部相机
           ├── SixCamSensor ×1  → JHH02 + JHH04 双通道
           ├── ImuSensor × N    → IMU 码带解码
           ├── CherryVideoSensor / CherrySerialSensor → cherry
           ├── EncoderSensor ×1 → AS5600
           └── ViveTracker ×1   → VIVE 姿态（检测到设备时）
```

每个 sensor 跑在独立 `std::thread`，三段式生命周期：`setup()` → `collect()` → `teardown()`；所有 sensor 在 `setup()` 完成后通过 `SimpleBarrier` 同步，同时进入采集。

### 视频管线（mango / banana 通用）

```
V4L2 (MJPEG) → turbojpeg 解码 → YUV
   ├── IMU：读 Y 平面顶部码带 → ImuFrameQueue → ImuSensor 异步写 JSONL
   └── 视频：YUV → NV12 → MPP H.265 硬编 → FIFO → ffmpeg → MKV
```

cherry 不走此管线，改为 `V4L2 H264 → ffmpeg stream-copy → MKV`。

---

## 3. 编译与运行

```bash
# 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc
```

服务启动前需配置两个文件：

```bash
install -d /etc/unified_capture
cp deploy/product.conf.example /etc/unified_capture/product.conf   # 选 mango/banana/cherry
cp deploy/camera-map.conf.example /etc/unified_capture/camera-map.conf
```

运行模式：

| 模式 | 命令 | 说明 |
|------|------|------|
| GPIO 按键 | `./unified_capture <prefix>` | 按键开始/停止 |
| Socket（systemd） | `./unified_capture --socket <prefix>` | 监听 socket 命令控制 |
| 启动即录 | `--no-gpio` | 启动立即采集 |
| 单次 | `--single` | 完成一次 session 后退出 |

命令行参数：`--scan`、`--no-gpio`、`--no-imu`、`--no-as5600`、`--no-h265`、`--socket`、`--single`、`--config <path>`。

---

## 4. Socket 控制协议

Unix Domain Socket，路径 `/tmp/unified_capture.sock`，纯文本 JSON，每行一条命令：

```bash
echo "status" | nc -U /tmp/unified_capture.sock   # 状态查询
echo "start"  | nc -U /tmp/unified_capture.sock   # 开始采集
echo "stop"   | nc -U /tmp/unified_capture.sock   # 停止采集
```

完整字段与语义见 [socket-control.md](socket-control.md)。

---

## 5. 输出数据概览

所有产物强制落在 SD 卡 `/media/usb0/capture/<prefix>/session_<NNN>/` 下。

| profile | 目录 | 产物 |
|---------|------|------|
| mango | `jhh02/ jhh04/ wrist_left/ wrist_right/` | MKV + IMU JSONL |
| banana | `jhh2_left/ jhh2_right/ jhh02/ jhh04/` + 顶层 `encoder-*.jsonl`、`tracker*.jsonl` | MKV + IMU/编码器/姿态 JSONL |
| cherry | `cherry_stereo/` | `cherry_stereo.mkv` + `video_frames/imu/mag/frame_meta` JSONL |

mango 的完整目录树、字段表与命名规则见 [mango-device-overview.md](mango-device-overview.md)。

---

## 6. 依赖（仅 RK3588 板端）

- **Linux UVC 驱动 (V4L2)** — 相机帧获取
- **Rockchip MPP** (`librockchip_mpp`) — H.265 硬件编码
- **libturbojpeg** — MJPEG 解码
- **libgpiod** — GPIO 按键
- **libsurvive** — VIVE Tracker（检测到设备时启用）
- **FFmpeg** — MKV 封装（运行时 fork+exec）

---

## 7. 相关文档

- [mango-device-overview.md](mango-device-overview.md) — mango 设备采集与输出详解
- [socket-control.md](socket-control.md) — Socket 控制协议
- [video-pipeline.md](video-pipeline.md) — 视频管线
- [device-ui-interface.md](device-ui-interface.md) — 前端接口定义
- [README.md](../README.md) — 项目总览
