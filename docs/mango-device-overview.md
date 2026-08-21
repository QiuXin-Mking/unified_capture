# Mango 设备采集与输出说明（双目档）

> 本文介绍杨总这套 ego 系列中的 **双目档**：设备组成、预期采集与输出，以及当前程序的支持情况。
>
> **命名说明**：ego 系列内部代号 `mango`，按「屏幕 / 摄像头」分四档。**本文只描述双目档（mango / mango plus）**；六目档（mango pro / mango pro plus）见 [mango-pro-device-overview.md](mango-pro-device-overview.md)。

| 规格 | 屏幕 | 头部摄像头 | 说明文档 |
|------|------|-----------|---------|
| mango pro plus | 有屏幕 | 六目 | [mango-pro-device-overview.md](mango-pro-device-overview.md) |
| mango pro | 无屏幕 | 六目 | [mango-pro-device-overview.md](mango-pro-device-overview.md) |
| mango plus | 有屏幕 | 双目 | 本文 |
| mango | 无屏幕 | 双目 | 本文 |

> 屏幕只影响交互形态，不影响采集链路与数据包结构；本文内容对 mango 与 mango plus 同样适用。

---

## 0. 支持状态（先看这一节）

**代码已实现，但尚未在板端验证。** 守护进程新增 `mango`（双目档）与 `mango_pro`（六目档）两个 profile：

- `mango`（双目档）：`discover_mango_cameras()` 取一块**不与 JHH04 同 bus** 的独立 JHH2（`1bcf:2d50`）作 `head`，加左右腕共 3 路。
- `mango_pro`（六目档）：原六目逻辑（JHH04 + JHH02 成对 + 双腕）原样迁移，函数改名 `discover_mango_pro_cameras()`。

> ⚠️ **板端验证待做**：V4L2 迁移后独立 JHH2 双目从未在 RK3588 实测（见 [Hardware/02-杨总双目摄像头.md](Hardware/02-杨总双目摄像头.md)）。本档代码已通过 host `make test` 与编译级自检，但**端到端采集尚未上板验收**，运行前必须先做板端 60 秒验证（实施计划 `docs/plans/2026-08-21-mango-mango-pro-profile-split.md` Task 10）。

本文其余章节描述的是**产品形态与预期数据包**。

---

## 1. 设备组成

mango 是一套**头戴 + 双腕**的多相机采集设备，由以下硬件组成：

| 硬件 | 接口 | 说明 |
|------|------|------|
| 独立双目模组 JHH2 | `1bcf:2d50` | 头部双目，3840×1200（左右拼接，各 1920×1200） |
| 左腕相机 | `SL`（可配置） | 左腕单目，1440×960 |
| 右腕相机 | `JHHSW`（可配置） | 右腕单目，1440×960 |

与六目档的差别只在头部：六目档是一块六目板（JHH02 双目侧 + JHH04 四目侧，共 6 镜头），双目档是一块独立 JHH2 模组（2 镜头）。腕部相机两档相同。

> **不含 AS5600 编码器、不含 VIVE Tracker** —— 这两者属于 umi（banana）方案。
>
> **待确认**：双目档头部是否为单块 JHH2 模组、物理镜头布局与左右对应，需现场复核（见 [Hardware/02-杨总双目摄像头.md](Hardware/02-杨总双目摄像头.md)）。

---

## 2. 能采集什么（3 路视频 + 3 路 IMU 码带）

| 相机 | VID/PID | 分辨率 | 帧率 | 码率 | 采集内容 |
|------|---------|--------|------|------|----------|
| 头部双目 JHH2 | `1bcf:2d50` | 3840×1200 | 30fps | 16 Mbps | H.265 视频 + IMU 码带 |
| 左腕 | `SL` | 1440×960 | 30fps | 8 Mbps | H.265 视频 + IMU 码带 |
| 右腕 | `JHHSW` | 1440×960 | 30fps | 8 Mbps | H.265 视频 + IMU 码带 |

**IMU 码带说明**：每个相机的图像**顶部有一条横向码带**，把 IMU 数据（ICM42688 加速度计 + 陀螺仪、AK09940 磁力计）编码成像素行嵌进视频帧。程序从每帧图像把这个码带解码出来，得到与视频逐帧对齐的 IMU 数据。三路相机各带一路码带，共 3 路 IMU。

---

## 3. 怎么实现（采集管线）

采集管线与六目档一致，只是头部换成一路 JHH2：

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

- **视频**：头部 JHH2 与腕部相机都取 MJPEG，由 `libturbojpeg` 解码成 YUV，转 NV12 交给 Rockchip MPP 做 H.265 硬编，编码流经 FIFO 喂给 ffmpeg `stream-copy` 封装成 MKV。双目档没有 JHH04，因此**不存在 YUYV 直取通道**。
- **IMU**：不阻塞视频链路。`VideoFrameProcessor` 每帧解码后从亮度平面读出码带原始字节投递到有界队列；`ImuSensor` 独立线程消费并解析成 JSONL。

### 启流顺序

双目档只有一路头部相机，没有六目板内部 JHH02→JHH04 的硬件依赖。预期顺序：

1. 头部 JHH2 先启流。
2. 左/右腕相机在头部启流后再启流。

仍由 `VideoCaptureControl` 协调。

> 注意：相同 VID/PID 的 UVC 设备存在并发流数量上限，头部 JHH2 与腕部同为 `1bcf:2d50` 系列时需实测确认并发是否受限。

### 运行模式

| 模式 | 触发 | 说明 |
|------|------|------|
| GPIO 按键 | `./unified_capture <prefix>` | 按键开始/停止录制 |
| Socket | `./unified_capture --socket <prefix>` | 监听 `/tmp/unified_capture.sock`，命令控制 |
| 启动即录 | `--no-gpio` | 启动立即采集 |
| 单次 | `--single` | 完成一次 session 后退出 |

---

## 4. 预期数据包

### 4.1 根路径规则

与六目档相同：

- 所有产物强制落在 SD 卡：`/media/usb0/capture/`（启动时校验挂载）。
- 相对输出前缀 → `/media/usb0/capture/<prefix>`；绝对路径必须在该目录下。
- 未指定前缀时，默认 `record_YYYYMMDD_HHMMSS`。
- 每次录制一个 session 目录：`session_001`、`session_002`…（`session_%03d`）。

### 4.2 数据包目录结构（预期，目录名待定）

```
/media/usb0/capture/<prefix>/session_001/
├── <头部双目目录>/                              # 3840×1200@30
│   ├── <name>-<ts>.mkv                        # H.265(HEVC) 视频
│   └── <name>-<ts>.jsonl                      # IMU（逐帧对齐）
├── wrist_left/                                 # 左腕 1440×960@30
│   ├── wrist_left-<ts>.mkv                    # H.265(HEVC) 视频
│   └── wrist_left-<ts>.jsonl                  # IMU
└── wrist_right/                                # 右腕 1440×960@30
    ├── wrist_right-<ts>.mkv
    └── wrist_right-<ts>.jsonl                 # IMU
```

时间戳 `<ts>` 格式：`YYYYMMDD-HH_MM_SS`，例如 `20260814-10_30_00`。

**待定项**：

- 头部双目目录名（banana 用 `jhh2_left` / `jhh2_right`，双目档只有一路，命名需另行确定）。
- 头部双目是否需要输出 `.y8`（banana 的 JHH2 输出 `.y8`；六目档只有 jhh04 输出 `.y8`）。

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

## 5. 实现落点（已完成，待板端验证）

双目档已按下列改造点落地（实现计划见 `docs/plans/2026-08-21-mango-mango-pro-profile-split.md`）：

| 位置 | 改动 |
|------|------|
| `core/product_config.h/.cpp` | `ProductProfile` 新增 `mango_pro`；`mango` 语义改为双目档 |
| `deploy/camera-map.conf.example` | 拆分 `[mango]`（双目）与 `[mango_pro]`（六目）两段 |
| `hardware/video/device_discovery.cpp` | 新增 `discover_mango_cameras()`（head + 双腕）；原六目改名 `discover_mango_pro_cameras()` |
| `app/session_runner.cpp` | 双目档建 head 的 `VideoSensor` + `ImuSensor`；六目逻辑迁入 `mango_pro` 分支 |
| `app/capture_output_policy.h` | `mango_camera_output_policy` 认 `head`；`mango_pro_camera_output_policy` 认 `jhh02`/`jhh04` |
| `app/session_profile.cpp` | `profile_cameras_json` 报 `head`；目录追加 `head/` |
| `app/runtime.cpp` | 严格模式期望数：双目档「腕 2 + 头 1」，六目档「腕 2 + 六目 2」 |
| `app/socket_server.cpp` | preview 通道白名单加 `head` |

---

## 6. 相关文档

- [mango-pro-device-overview.md](mango-pro-device-overview.md) — 六目档（mango pro / mango pro plus）
- [unified_capture-项目介绍.md](unified_capture-项目介绍.md) — 产品向介绍（四档规格）
- [unified_capture-overview.md](unified_capture-overview.md) — 整体程序介绍（三套产品方案）
- [Hardware/02-杨总双目摄像头.md](Hardware/02-杨总双目摄像头.md) — 独立 JHH2 双目模组硬件资料
- [../README.md](../README.md) — 项目总览与输出结构
