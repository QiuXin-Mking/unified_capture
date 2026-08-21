# Mango Pro 设备采集与输出说明（六目档）

> 本文介绍杨总这套 ego 系列中的 **六目档**：能采集什么、能输出什么、如何实现、最终交付的数据包长什么样。
>
> **命名说明**：ego 系列内部代号 `mango`，按「屏幕 / 摄像头」分四档。**本文只描述六目档（mango pro / mango pro plus）**；双目档（mango / mango plus）见 [mango-device-overview.md](mango-device-overview.md)。

| 规格 | 屏幕 | 头部摄像头 | 说明文档 |
|------|------|-----------|---------|
| mango pro plus | 有屏幕 | 六目 | 本文 |
| mango pro | 无屏幕 | 六目 | 本文 |
| mango plus | 有屏幕 | 双目 | [mango-device-overview.md](mango-device-overview.md) |
| mango | 无屏幕 | 双目 | [mango-device-overview.md](mango-device-overview.md) |

> 屏幕只影响交互形态，不影响采集链路与数据包结构；本文内容对 mango pro 与 mango pro plus 同样适用。

---

## 1. 设备组成

mango pro 是一套**头戴 + 双腕**的多相机采集设备，由以下硬件组成：

| 硬件 | 接口 | 说明 |
|------|------|------|
| 六目模组 JHH02（双目侧） | `1bcf:2d50` | 头部双目，4000×1200 |
| 六目模组 JHH04（四目侧） | `1bcf:2d51` | 头部四目，3104×480 |
| 左腕相机 | `SL`（可配置） | 左腕单目，1440×960 |
| 右腕相机 | `JHHSW`（可配置） | 右腕单目，1440×960 |

> JHH02 与 JHH04 是**同一块六目板**上的两个通道，共 6 个镜头，共享 IMU 硬件。
>
> **不含 AS5600 编码器、不含 VIVE Tracker** —— 这两者属于 umi（banana）方案。

---

## 2. 能采集什么（4 路视频 + 4 路 IMU 码带）

| 相机 | VID/PID | 分辨率 | 帧率 | 采集内容 |
|------|---------|--------|------|----------|
| JHH02（双目侧） | `1bcf:2d50` | 4000×1200 | 30fps | H.265 视频 + IMU 码带 |
| JHH04（四目侧） | `1bcf:2d51` | 3104×480 | 30fps | 视频 + IMU 码带 |
| 左腕 | `SL` | 1440×960 | 30fps | H.265 视频 + IMU 码带 |
| 右腕 | `JHHSW` | 1440×960 | 30fps | H.265 视频 + IMU 码带 |

**IMU 码带说明**：每个相机的图像**顶部有一条横向码带**，把 IMU 数据（ICM42688 加速度计 + 陀螺仪、AK09940 磁力计）编码成像素行嵌进视频帧。程序从每帧图像把这个码带解码出来，得到与视频逐帧对齐的 IMU 数据。四路相机各带一路码带，共 4 路 IMU。

---

## 3. 怎么实现（采集管线）

```
V4L2 采集 (MJPEG 帧)
   │
   ├── MJPEG 解码 → YUV
   │      ├── IMU 码带提取：读 Y(亮度) 平面顶部行 → 推入 ImuFrameQueue（异步）
   │      │        └── ImuSensor 独立线程 → 解析码带 → 写 IMU JSONL
   │      └── 视频编码：YUV → NV12 → MPP H.265 硬件编码
   │               └── FIFO → ffmpeg 子进程 → 封装 MKV
   └── 预览 JPEG（按需，socket preview 命令触发）
```

关键点：

- **视频**：JHH04 四目经内核 UVC/V4L2 获取 YUYV，直接拆出 Y/U/V 平面；JHH02 和腕部相机仍获取 MJPEG 并由 `libturbojpeg` 解码成 YUV。两类输入统一转 NV12 交给 Rockchip MPP 做 H.265 硬编，编码流经 FIFO 喂给 ffmpeg `stream-copy` 封装成 MKV。
- **IMU**：不阻塞视频链路。`VideoFrameProcessor` 每帧解码后从亮度平面读出码带原始字节投递到有界队列；`ImuSensor` 独立线程消费并解析成 JSONL。
- 腕部相机同样走 MJPEG→H.265 管线，也嵌 IMU 码带；不生成 Y8 灰度。

### 设备发现（六目成对约束）

`discover_mango_cameras()` 先按 `1bcf:2d51` 找到 JHH04 并记下它所在的 USB bus，再在**同一 bus** 上找 `1bcf:2d50` 的 JHH02。两者缺任一个，六目整体判为不可用（`sixcam.enabled = false`），头部就没有相机——**六目档必须两个通道同时在位**。

### 启流顺序

1. 六目模组先启流（`SixCamSensor` 内部先 JHH02 后 JHH04）。
2. 左/右腕相机在六目启流后再启流。

`VideoCaptureControl` 负责协调这段依赖关系。

### 运行模式

| 模式 | 触发 | 说明 |
|------|------|------|
| GPIO 按键 | `./unified_capture <prefix>` | 按键开始/停止录制 |
| Socket | `./unified_capture --socket <prefix>` | 监听 `/tmp/unified_capture.sock`，命令控制 |
| 启动即录 | `--no-gpio` | 启动立即采集 |
| 单次 | `--single` | 完成一次 session 后退出 |

---

## 4. 最终交付数据包

### 4.1 根路径规则

- 所有产物强制落在 SD 卡：`/media/usb0/capture/`（启动时校验挂载）。
- 相对输出前缀 → `/media/usb0/capture/<prefix>`；绝对路径必须在该目录下。
- 未指定前缀时，默认 `record_YYYYMMDD_HHMMSS`。
- 每次录制一个 session 目录：`session_001`、`session_002`…（`session_%03d`）。

### 4.2 数据包目录结构

```
/media/usb0/capture/<prefix>/session_001/
├── jhh02/                                      # 六目双目侧 4000×1200@30
│   ├── jhh02-<ts>.mkv                         # H.265(HEVC) 视频
│   └── jhh02-<ts>.jsonl                       # IMU（逐帧对齐）
├── jhh04/                                      # 六目四目侧 3104×480@30
│   ├── jhh04-<ts>.mkv                         # H.265(HEVC) 视频
│   ├── jhh04-<ts>.y8                          # 无头 Y8 原始帧，逐帧 3104×480
│   └── jhh04-<ts>.jsonl                       # IMU（逐帧对齐）
├── wrist_left/                                 # 左腕 1440×960@30
│   ├── wrist_left-<ts>.mkv                    # H.265(HEVC) 视频
│   └── wrist_left-<ts>.jsonl                  # IMU
└── wrist_right/                                # 右腕 1440×960@30
    ├── wrist_right-<ts>.mkv
    └── wrist_right-<ts>.jsonl                 # IMU
```

时间戳 `<ts>` 格式：`YYYYMMDD-HH_MM_SS`，例如 `20260814-10_30_00`。

### 4.3 各文件格式

#### 视频 `.mkv`
- 容器 MKV，视频流 H.265（HEVC）。
- MPP 硬件编码，ffmpeg `-f hevc -c copy` 直通封装，不二次转码。

#### IMU `.jsonl`（每行一个 JSON，两类样本混写）

加速度 / 陀螺仪样本（ICM42688）：
```json
{"frame_idx":123,"t_us":45678,"ax_mg":-12.345,"ay_mg":0.123,"az_mg":998.7,"gx_mdps":-10.2,"gy_mdps":0.5,"gz_mdps":1.1,"exp_start_us":1000,"exp_end_us":2000}
```

磁力计样本（AK09940）：
```json
{"frame_idx":123,"t_us":45678,"magx_ut":-12.3,"magy_ut":45.6,"magz_ut":78.9,"mag_temp_c":25.0,"exp_start_us":1000,"exp_end_us":2000}
```

| 字段 | 含义 |
|------|------|
| `frame_idx` | 视频帧序号，用于与视频对齐 |
| `t_us` | 码带内样本时间戳 |
| `ax/ay/az_mg` | 加速度（mg，毫 g） |
| `gx/gy/gz_mdps` | 角速度（mdps，毫度/秒） |
| `magx/y/z_ut` | 磁力计（µT，微特斯拉） |
| `mag_temp_c` | 磁力计温度（℃） |
| `exp_start_us` / `exp_end_us` | 曝光窗口（帧级） |

---

## 5. jhh04 算法侧读取

`jhh04` 使用 `3104×480@30` 的 YUYV 输入，同时输出 H.265 MKV 和无头 Y8 原始帧。YUYV 按 `Y0 U Y1 V` 拆分，Y8 直接复制其中的 Y 字节，不包含行填充或文件头；算法侧按 `gray8`、`3104×480`、`30fps` 读取，每帧占 `3104×480` 字节。两种输出来自同一次 YUYV 解包和同一帧序列。

---

## 6. 相关文档

- [mango-device-overview.md](mango-device-overview.md) — 双目档（mango / mango plus）
- [unified_capture-项目介绍.md](unified_capture-项目介绍.md) — 产品向介绍（四档规格）
- [unified_capture-overview.md](unified_capture-overview.md) — 整体程序介绍（三套产品方案）
- [Hardware/03-杨总六目摄像头.md](Hardware/03-杨总六目摄像头.md) — 六目模组硬件资料
- [../README.md](../README.md) — 项目总览与输出结构
