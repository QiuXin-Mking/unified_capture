# BUG：腕部相机 — 已连接但不被识别（product.conf=mango 忽略 2d52）

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已修复 |
| 严重级别 | Medium |
| 首次发现 | 2026-08-13 |
| 最后更新 | 2026-08-13 |
| 负责人 | unified_capture 团队 |
| 影响版本 | unified_capture binary（08-12 18:17）+ device-ui |
| 关联任务 | 无 |

> **目标：** 记录腕部相机（SL/JHHSW）已物理连接但守护进程不识别、UI 显示无信号的根因与恢复方式。

---

## 现象

两个腕部相机（`1bcf:2d52`，`SL` / `JHHSW`）已通过 USB 连接，但：

- UI 展示功能里「左腕部单目」「右腕部单目」两路显示「无信号」。
- 守护进程 `status` 的 `cameras` 字段只有 `jhh02` / `jhh04`（头部六目），没有 `wrist_left` / `wrist_right`。

## 影响

- 影响功能：腕部相机预览与采集。
- 影响设备或数据：腕部 SL/JHHSW 相机数据不采集。
- 是否阻塞发布或交付：是（腕部为交付验收项）。
- 临时恢复方式：将 `product.conf` 改为 `banana` 并重启 `unified_capture.service`。

## 环境

| 项目 | 值 |
|------|----|
| 硬件 | RK3588（hostname `rk`） |
| 操作系统 / 内核 | Debian |
| 软件提交 | unified_capture binary 08-12 18:17 |
| SDK / 驱动 | Linux UVC (V4L2) |
| 设备连接 | `1bcf:2d52` SL、`1bcf:2d52` JHHSW（腕部）、`1bcf:2d50` jhh02、`1bcf:2d51` jhh04（六目） |
| 启动参数 | `/usr/local/bin/unified_capture --socket --single /media/usb0/capture` |
| 配置文件 | `/etc/unified_capture/product.conf` = `product=mango`（错误值） |

## 复现步骤

### Step 1：product.conf 配成 mango

**操作：**

```bash
echo "product=mango" > /etc/unified_capture/product.conf
systemctl restart unified_capture.service
```

**预期：** 守护进程按 mango（legacy_head）模式启动。

### Step 2：连接腕部相机并查 status

**操作：**

```bash
lsusb | grep 1bcf
echo status | nc -U /tmp/unified_capture.sock
```

**预期：** `status.cameras` 包含 `wrist_left` / `wrist_right`。

**实际：** `cameras` 只有 `jhh02` / `jhh04`，无腕部。

## 预期结果

物理连接 4 台相机（2 腕部 + 2 六目）时，`status.cameras` 返回 `wrist_left`、`wrist_right`、`jhh02`、`jhh04` 全部为 `true`。

## 实际结果

`product=mango` 时腕部相机被 V4L2 枚举到但被丢弃，`status.cameras` 无腕部；UI 腕部两路显示「无信号」。

## 证据

### 关键日志

```text
# 物理设备都在
$ lsusb | grep 1bcf
Bus 006 Device 007: ID 1bcf:2d51 ... JHH
Bus 006 Device 006: ID 1bcf:2d50 ... JHH
Bus 002 Device 006: ID 1bcf:2d52 ... JHHSW
Bus 002 Device 005: ID 1bcf:2d52 ... SL

# 配置错误
$ cat /etc/unified_capture/product.conf
product=mango

# status 无腕部
$ echo status | nc -U /tmp/unified_capture.sock
{"ok":true,"product":"mango","ready":true,...,"cameras":{"jhh2_left":false,"jhh2_right":false,"jhh04":true,"jhh02":true},...}

# 枚举日志：认到 4 台，但只分配六目
V4L2: found 4 device(s)
  /dev/video2: 1bcf:2d52 bus=2 product="JHHSW"
  /dev/video0: 1bcf:2d52 bus=2 product="SL"
  jhh04 -> /dev/video6 bus=6 ...
  jhh02 -> /dev/video4 bus=6 ...
  jhh2_left  -> disabled (no free 2d50 on other buses)
  jhh2_right -> disabled (no free 2d50 on other buses)
```

### 实验结果

| 条件 | 结果 | 结论 |
|------|------|------|
| `product=mango` + 4 台相机在线 | `cameras` 无 `wrist_left`/`wrist_right` | 腕部被忽略 |
| `product=banana` + 重启 | `cameras.wrist_left=true, wrist_right=true` | 腕部被识别 |

## 根因分析

**结论状态：** 已确认

`device_discovery.cpp` 的 `discover_cameras()` 按 profile 派发：仅 `banana` 走 `discover_banana_cameras()` → `match_wrist_cameras()` 匹配 `wrist_left.product=SL`、`wrist_right.product=JHHSW`；`mango` 走 `discover_mango_cameras()`，只查找 `1bcf:2d51`(jhh04) 与 `1bcf:2d50`(jhh02)，**完全不处理 `1bcf:2d52` 腕部设备**。

更深层是**跨层命名错位**：device-ui 前端的「Mango」产品 = 头部 Ego + 左腕 + 右腕，实际对应守护进程的 `banana` profile；而守护进程的 `mango` 是 `legacy_head`（只有头部六目）。前端选「Mango」时误配 `product=mango`，导致腕部不被采集。

## 解决或规避方案

### 当前方案

```bash
cp /etc/unified_capture/product.conf /etc/unified_capture/product.conf.bak.mango
echo "product=banana" > /etc/unified_capture/product.conf
systemctl restart unified_capture.service
```

### 风险与限制

- 改 `banana` 后按 CLAUDE.md 会输出腕部 H.265 MKV 与 IMU JSONL；若腕部相机不内嵌 IMU 码带，IMU 输出可能为空（见 [[wrist-camera-no-imu]] 相关说明）。
- 前端「Mango」与守护进程 `banana` 的命名错位是根本隐患，已在 `CLAUDE.md` 项目概述中补充警告。

## 验证结果

**验证状态：** 已通过

| 验证项 | 操作 | 预期 | 实际 | 结论 |
|--------|------|------|------|------|
| 守护进程识别腕部 | `status` | `wrist_left/right=true` | `wrist_left=true, wrist_right=true` | 通过 |
| UI 状态同步 | `GET /api/record/status` | cameras 含腕部 | `{"wrist_left":true,"wrist_right":true,"jhh04":true,"jhh02":true}` | 通过 |

## 相关文件

- `/etc/unified_capture/product.conf` — 产品 profile 配置（mango/banana/cherry）
- `hardware/video/device_discovery.cpp` — `discover_cameras()` / `discover_mango_cameras()` / `discover_banana_cameras()` 派发
- `CLAUDE.md` — 项目概述中的「前端 Mango ↔ 守护进程 banana」命名映射警告

## 经验教训

1. 相机「已连接但不识别」时，先确认 `product.conf` 的 profile 是否与硬件/前端产品匹配；`mango`（legacy_head）不含腕部，腕部相机只有 `banana` 会匹配。
2. 前端产品名与守护进程 profile 名不一致（前端「Mango」= 守护进程 `banana`），排查跨层问题时不要按同名假设对应关系。

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-08-13 | unified_capture 团队 | 首次记录 |
