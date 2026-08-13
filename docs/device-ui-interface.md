# unified_capture 接口定义（供 device-ui 对接）

> 本文是 `unified_capture` 守护进程对外暴露的**完整接口契约**，供 device-ui 独立开发对接使用。只描述「如何调用」和「会得到什么」，不涉及内部实现。
>
> 内部实现细节见 [`socket-control.md`](socket-control.md)；视频/IMU 数据格式见 [`video-pipeline.md`](video-pipeline.md)。

---

## 1. 概述

`unified_capture` 是 RK3588 上的**统一采集守护进程**，负责：

- 枚举 USB 相机（UVC/V4L2）
- 启动/停止采集会话（session）
- 输出视频（H.265 MKV）、IMU（JSONL）、编码器（AS5600）、VIVE 姿态数据到 SD 卡
- 按需导出相机预览 JPEG

device-ui 通过 **Unix Domain Socket** 与它通信（控制），并通过**文件系统**读取采集结果。

### 部署形态

```bash
# systemd 服务（常驻）
/usr/local/bin/unified_capture --socket --single /media/usb0/capture
```

| 参数 | 含义 |
|------|------|
| `--socket` | 进入 Socket 控制模式（不读 GPIO 按键） |
| `--single` | 完成一次 session 后进程退出（由 systemd `Restart=always` 自动拉起） |
| 位置参数 | 输出前缀，session 目录写在其下（本机为 `/media/usb0/capture`） |

---

## 2. 通信协议（Transport）

| 项目 | 值 |
|------|-----|
| 路径 | `/tmp/unified_capture.sock` |
| 类型 | `AF_UNIX` + `SOCK_STREAM` |
| 连接模式 | **短连接**：每次新建连接 → 发一条命令 → 收一条响应 → 关闭 |
| 请求格式 | UTF-8 文本，以换行 `\n` 结尾 |
| 响应格式 | 单行 JSON，以换行 `\n` 结尾 |
| 可用性判断 | socket 存在且 `status` 返回 `"ok":true`，即认为守护进程可用 |

---

## 3. 命令定义

所有响应都至少包含 `ok`（boolean）。错误时附带 `error`（string，见各命令）。

### 3.1 `status` — 查询状态

```text
status\n
```

响应（完整示例）：

```json
{
  "ok": true,
  "product": "banana",
  "ready": true,
  "degraded": false,
  "running": false,
  "session": null,
  "elapsed_ms": 0,
  "cameras": {
    "wrist_left": true,
    "wrist_right": true,
    "jhh04": true,
    "jhh02": true
  },
  "camera_errors": [],
  "imu": true,
  "as5600": false,
  "vive": false
}
```

字段说明：

| 字段 | 类型 | 说明 |
|------|------|------|
| `ok` | boolean | 恒为 `true`（socket 可达即 true） |
| `product` | string | 产品 profile：`mango` / `banana` / `cherry`（由 `/etc/unified_capture/product.conf` 决定） |
| `ready` | boolean | 设备枚举和外设检测是否完成 |
| `degraded` | boolean | 是否降级运行（允许缺设备时的标志） |
| `running` | boolean | 是否有一个 session 正在采集 |
| `session` | null | 恒为 `null`，**不要依赖** session 名称 |
| `elapsed_ms` | integer | 当前 session 已运行时长（毫秒）；空闲时为 `0` |
| `cameras` | object | 各相机在线状态，key 集合随 profile 变化（见 §4） |
| `camera_errors` | string[] | 设备发现阶段的错误信息（可为空数组） |
| `imu` | boolean | IMU 功能是否启用 |
| `as5600` | boolean | AS5600 编码器是否启用 |
| `vive` | boolean | VIVE Tracker 是否启用 |

> **热插拔**：`status` 在**空闲（非 running）时每次都会重新扫描 USB 设备**，所以拔插相机后，下一次 `status` 就能拿到最新的在线/离线状态。

### 3.2 `start` — 开始采集

```text
start\n
```

| 条件 | 响应 |
|------|------|
| 未就绪 | `{"ok":false,"error":"not ready"}` |
| 已在采集 | `{"ok":false,"error":"already running"}` |
| 空闲且就绪 | `{"ok":true}` |

`{"ok":true}` 仅表示**请求已受理**，不表示已经开流。主循环在**下一个整秒**才真正启动 session，调用方应轮询 `status.running` 确认。

### 3.3 `stop` — 停止采集

```text
stop\n
```

| 条件 | 响应 |
|------|------|
| 正在采集 | `{"ok":true,"elapsed_ms":0}` |
| 空闲 | `{"ok":false,"error":"not running"}` |
| 已有停止请求 | `{"ok":false,"error":"stop already scheduled"}` |

`stop` 会**阻塞到 session 完全结束**（视频收尾、MKV 封装完成）才返回。收到 `{"ok":true}` 后，采集文件已落盘完整。

> ⚠️ `elapsed_ms` 当前实现恒为 `0`，不要依赖它取时长；时长请由调用方自己计时。

### 3.4 `preview:<channel>:<path>` — 导出预览 JPEG

**通道格式（推荐）：**

```text
preview:jhh02:/tmp/camera_preview_jhh02.jpg\n
```

- `<channel>` ∈ `jhh02` | `jhh04` | `wrist_left` | `wrist_right`
- `<path>` 必须是**绝对路径**（以 `/` 开头）

**legacy 单路径格式：**

```text
preview:/tmp/preview.jpg\n
```

| 条件 | 响应 |
|------|------|
| 正在采集 | `{"ok":true}` |
| 空闲 | `{"ok":false,"error":"not running"}` |
| 非法通道或非绝对路径 | `{"ok":false,"error":"unknown command"}` |

`{"ok":true}` 仅表示**导出请求已登记**；JPEG 由采集线程在**后续帧**写到 `<path>`（先写 `<path>.tmp` 再 `rename` 原子落盘）。调用方在读取前应确认文件已生成且非空。

### 3.5 未知命令

```text
reboot\n
```

```json
{"ok":false,"error":"unknown command"}
```

---

## 4. `cameras` 字段（profile 相关）

`cameras` 的 key 集合由 `product.conf` 的 profile 决定：

| profile | `cameras` 的 key |
|---------|------------------|
| `mango`（legacy_head） | `jhh02`、`jhh04`、`jhh2_left`、`jhh2_right` |
| `banana`（腕部+六目） | `jhh02`、`jhh04`、`wrist_left`、`wrist_right` |
| `cherry` | `cherry_stereo` |

- key 是**固定名**，value 是 boolean（该路相机是否在线）。
- 未检测到的通道**不会出现在对象中**（所以判断某路时用 `cameras[key] === true`，而不是 `!== false`）。
- 预览通道名（§3.4 的 `<channel>`）与 `cameras` 的 key 一致。

---

## 5. 会话生命周期

```text
                 ┌───────────── 空闲 ─────────────┐
                 │  running=false, ready=true     │
                 └──────────────┬────────────────┘
                                │ start（下个整秒开流）
                                ▼
                 ┌───────────── 采集中 ───────────┐
                 │  running=true                  │
                 │  → 写 MKV/JSONL 到 session 目录 │
                 │  → preview 命令可导出 JPEG      │
                 └──────────────┬────────────────┘
                                │ stop（阻塞到收尾完成）
                                ▼
                 ┌───────────── 回到空闲 ─────────┐
                 │  --single：进程退出，systemd 拉起│
                 └────────────────────────────────┘
```

关键规则：

1. **`preview` 只在 `running=true` 时可用**；没有「仅预览不落盘」模式。
2. **「实时预览」是上层实现的**：先 `start` 一个 session → 用 `preview` 抽帧 → 最后 `stop` 并删除该 session。守护进程不区分「预览 session」和「正式录制 session」。
3. `start` 成功后轮询 `status.running` 直到 `true`；`stop` 成功后轮询 `status.running` 直到 `false`。
4. `--single` 模式下，session 结束后进程退出，systemd 重启，重启窗口（约 2~4 秒）内 socket 不可达，调用方需容忍 `unreachable`/超时。

---

## 6. 输出文件布局

采集结果写在 `<输出前缀>/session_NNN/`（`NNN` 从 001 递增，重启后从已有最大值 +1 继续）。

`banana` profile 的 session 目录：

```
/media/usb0/capture/session_050/
├── jhh02/         → jhh02-<时间戳>.mkv（H.265）+ jhh02-<时间戳>.jsonl（IMU）
├── jhh04/         → jhh04-<时间戳>.jsonl（IMU）
├── wrist_left/    → wrist_left-<时间戳>.mkv + wrist_left-<时间戳>.jsonl
├── wrist_right/   → wrist_right-<时间戳>.mkv + wrist_right-<时间戳>.jsonl
├── encoder.jsonl  → AS5600 编码器（若启用）
└── tracker.jsonl  → VIVE 姿态（若启用）
```

- 时间戳格式：`YYYYMMDD-HH_MM_SS`。
- 各相机的文件只在其在线时产生；某路相机缺失/离线时，对应子目录不创建。
- 视频为 H.265 编码的 MKV；IMU 为 JSONL（每行一个 JSON 对象，schema 见 [`video-pipeline.md`](video-pipeline.md)）。

---

## 7. 调用示例（完整流程）

```bash
SOCK=/tmp/unified_capture.sock

# 1. 查询状态
echo 'status' | nc -U "$SOCK"

# 2. 开始采集
echo 'start' | nc -U "$SOCK"

# 3. 等待真正进入采集中
while ! echo 'status' | nc -U "$SOCK" | grep -q '"running":true'; do sleep 0.1; done

# 4. 导出预览帧（采集中）
echo 'preview:jhh02:/tmp/preview_jhh02.jpg' | nc -U "$SOCK"

# 5. 停止采集（阻塞到收尾完成）
echo 'stop' | nc -U "$SOCK"

# 6. 确认回到空闲
while echo 'status' | nc -U "$SOCK" | grep -q '"running":true'; do sleep 0.1; done
```

---

## 8. 变更记录

| 日期 | 内容 |
|------|------|
| 2026-08-13 | 首次整理对外接口定义（从 `socket-control.md` 抽取，补充热插拔/文件布局） |
