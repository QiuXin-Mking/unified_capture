# Mango / Mango Pro 双 Profile 拆分设计（双目档 vs 六目档）

> 状态: **设计已确认（brainstorm 完毕，待实施）**
> 日期: 2026-08-21
> 关联: [unified_capture-项目介绍.md](../unified_capture-项目介绍.md)、[mango-device-overview.md](../mango-device-overview.md)、[mango-pro-device-overview.md](../mango-pro-device-overview.md)

## 1. 背景与目标

产品侧 ego 系列按「屏幕 × 摄像头」分四档：

| 规格 | 屏幕 | 摄像头 |
|------|------|--------|
| mango pro plus | 有屏幕 | 六目 |
| mango pro | 无屏幕 | 六目 |
| mango plus | 有屏幕 | 双目 |
| mango | 无屏幕 | 双目 |

当前 daemon 只有三个 profile（`mango` / `banana` / `cherry`），其中 `mango` 语义是「六目 + 双腕」，**无法表达「双目档」（单块独立 JHH2 双目 + 双腕）**——独立 JHH2 的发现/采集路径只存在于 `banana`。本设计把 ego 两档落进 daemon 的两个 profile，并让双目档真正能采集。

**目标**：
1. 用 `mango`（双目档）与 `mango_pro`（六目档）两个平级 profile 表达 ego 两档。
2. 双目档落地单块独立 JHH2 双目的发现与采集。
3. `banana` / `cherry` 保持不动。

## 2. 已确认的关键决策

| 决策点 | 结论 |
|--------|------|
| 架构方式 | 方案 A：两个平级 profile（`mango` / `mango_pro`） |
| 双目档头部硬件 | 单块独立 JHH2 双目（`1bcf:2d50`，3840×1200@30，左右两镜拼一张宽幅） |
| banana 去留 | 保留不动（JHH2×2 + 六目 + AS5600 + VIVE） |
| 双目档头部输出 | H.265 MKV + IMU JSONL，**无 Y8** |
| 双目档头部目录名 | `head/` |
| JHH2 采集复用 | 复用 `banana` 现有 JHH2 发现与 VideoSensor/IMU 采集逻辑，不重写 |

## 3. 最终 profile 全景（4 个）

| profile | 头部 | 腕部 | 其他 |
|---|---|---|---|
| `mango` | 独立 JHH2 双目 ×1 → `head/` | SL + JHHSW | 无 AS5600/VIVE/六目 |
| `mango_pro` | 六目 JHH02+JHH04 → `jhh02/` `jhh04/` | SL + JHHSW | 无 |
| `banana` | JHH2 独立双目 ×2 + 六目 | — | AS5600 + VIVE |
| `cherry` | YCTC SC233HGS 双目 | — | CDC ACM 串口 |

> ⚠️ **破坏性变更**：`mango` 语义从「六目 + 双腕」改为「双目 + 双腕」。现有板子 `product.conf` 若写 `product=mango` 且接六目，须改为 `product=mango_pro`。

## 4. 配置层

### 4.1 `product.conf`

值域从 `mango|banana|cherry` 扩展为 `mango|mango_pro|banana|cherry`。

### 4.2 `camera-map.conf`

```ini
[mango]              # 双目档
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW
# head 硬编码为 JHH2（1bcf:2d50，3840×1200@30），无需在此声明

[mango_pro]          # 六目档（原 [mango] 段原样迁移）
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW
sixcam.enabled=true
```

- 双目档头部 JHH2 的 VID/PID/分辨率复用代码常量 `kJhh2Vid` / `kJhh2Pid` / 3840×1200，与 banana 一致，不新增配置项。
- `mango_pro` 即原 `[mango]` 段改名，解析逻辑不动。

## 5. 代码改动地图

| 模块 | 改动 |
|---|---|
| `core/product_config.h` | `enum class ProductProfile` 加 `mango_pro`；`ProductConfiguration` 的 `sixcam_enabled` 改为仅 mango_pro 使用 |
| `core/product_config.cpp` | `parse_product_profile` / `product_profile_name` / `write_product_profile` / `load_*_for_profile` 加 `mango_pro`；`load_product_configuration_for_profile` 分 `mango`（双目）与 `mango_pro`（六目）两分支 |
| `hardware/video/device_discovery.h/.cpp` | `CameraDiscoveryResult` 加 `head` 槽；新增 `discover_mango_cameras()`（1 块独立 JHH2 + 双腕）与 `discover_mango_pro_cameras()`（原六目逻辑） |
| `app/session_runner.cpp` | `mango` 分支：1 个 `VideoSensor`（head，H.265+IMU 无 Y8）+ 双腕；`mango_pro` 分支：现六目逻辑 |
| `app/capture_output_policy.h` | `mango_camera_output_policy` 认 `head`；mango_pro 认 `jhh02`/`jhh04` |
| `app/session_profile.cpp` | `active_profile_cameras` / `profile_cameras_json` / `profile_session_directories` 按 profile 分流，mango 上报 `head` |
| `app/runtime.cpp` | 严格模式期望数：mango = 腕 2 + 头 1，mango_pro = 腕 2 + 六目 2；preview channel 加 `head` |
| `app/status_response.cpp` / socket | `product` 上报 `mango` / `mango_pro`；preview 支持 `head` 通道 |

**复用点**：双目档 JHH2 采集复用 `discover_banana_cameras()` 的 JHH2 分配逻辑（`device_discovery.cpp:262` 起，选「不与 JHH04 同 bus 的 `1bcf:2d50`」）与 `session_runner.cpp:152` 起的 VideoSensor + ImuSensor 创建代码，不重写。

## 6. 数据流与启流顺序（双目档）

```
发现: 扫描 V4L2 → 找 1 块不与 JHH04(2d51) 同 bus 的 JHH2(2d50) → head
      → 匹配 SL / JHHSW → wrist_left / wrist_right
采集: head (VideoSensor, MJPEG→H.265+IMU) + 双腕 (VideoSensor, MJPEG→H.265+IMU)
启流: head 先启流 → 双腕再启流（无六目内部 JHH02→JHH04 硬件依赖）
输出:
  session_001/
    ├── head/          head-<ts>.mkv + head-<ts>.jsonl
    ├── wrist_left/    wrist_left-<ts>.mkv + wrist_left-<ts>.jsonl
    └── wrist_right/   wrist_right-<ts>.mkv + wrist_right-<ts>.jsonl
```

**已知风险**：V4L2 迁移（2026-07-29）后，独立 JHH2 双目**从未在板端验证**（见 `docs/Hardware/02-杨总双目摄像头.md`、`docs/TODO.md`）。双目档落地后必须上板验收，不能沿用迁移前的 Nori SDK 实测结论。

## 7. 测试与验收

### 7.1 host 单元/回归（无硬件）

`make test` 补：
- `test_product_config`：`mango_pro` 解析、`mango` 双目配置加载。
- `test_session_profile`：mango 上报 `head`，mango_pro 上报 `jhh02`/`jhh04`。
- `test_capture_output_policy`：`head` → `{H.265, 无 Y8}`。
- `test_status_response`：`product` 字段值。

### 7.2 板端验收（RK3588）

1. 双目档：接 1 块 JHH2 + 双腕 → `--scan` 认到 head + 双腕 → 60 秒采集 → `head-*.mkv`（HEVC 3840×1200）+ `head-*.jsonl` + 双腕，30fps、0 丢帧、`queue_overflows=0`。
2. 六目档回归：接六目 + 双腕 → `product=mango_pro` 仍四路 30fps（对照 `docs/records/v4l2-30fps-jhh02-h265-status.md`）。

### 7.3 前端联动（另立任务）

device-ui 产品选择 → 四档映射到 `mango` / `mango_pro`，`set_product` 热切换、`status.product` 正确显示。

## 8. 非目标（YAGNI）

- 不改 `banana` / `cherry` 行为。
- 双目档不引入 AS5600 / VIVE / 六目。
- 不做「一块板子同时跑双目和六目」的混合档。
- 屏幕（有屏 / 无屏）差异不落 daemon，仅前端呈现，daemon 不感知。
