# unified_capture — Socket 控制协议

> Unix Domain Socket 控制接口参考
>
> 本文以当前 `app/runtime.cpp` 与 `app/socket_server.cpp` 实现为准。

## 架构与生命周期

`unified_capture` 使用单线程 `poll()` 同时处理 GPIO 和 Socket；没有独立的 Socket 线程。主线程是 session 生命周期的唯一 owner：Socket 命令只更新控制状态或返回状态，不直接调用 `run_session()`。

```text
device-ui / 运维脚本
        │ AF_UNIX SOCK_STREAM，短连接
        ▼
/tmp/unified_capture.sock
        │
        ▼
main 线程 poll()
  ├─ start  → 标记启动请求；主循环创建 session
  ├─ stop   → g_session_running = false；session 随后清理
  ├─ status → 返回当前状态
  └─ preview:<path> → 请求采集线程导出下一张预览 JPEG
```

程序完成设备枚举和外设检测后才建立 Socket 并将 `ready` 置为 `true`。Socket 模式使用 `--socket`，此时不初始化 GPIO；GPIO 模式也会建立同一 Socket，可同时接受 Socket 控制。

## 连接规范

| 项目 | 值 |
|------|-----|
| 路径 | `/tmp/unified_capture.sock` |
| 类型 | `AF_UNIX` + `SOCK_STREAM` |
| 连接模式 | 短连接：每个连接发送一条命令，收到一条响应后关闭 |
| 请求格式 | UTF-8 纯文本，以换行符 `\n` 结束 |
| 响应格式 | 单行 JSON，以换行符 `\n` 结束 |
| 残留 Socket | 启动时会探测并删除无效的旧 Socket 文件 |
| 退出清理 | 关闭监听 fd 并删除 Socket 文件 |

## 命令协议

### `start`

```text
start\n
```

| 条件 | 响应 | 含义 |
|------|------|------|
| 未就绪 | `{"ok":false,"error":"not ready"}` | 设备尚未完成初始化 |
| 正在采集 | `{"ok":false,"error":"already running"}` | 不创建第二个 session |
| 空闲且已就绪 | `{"ok":true}` | 启动请求已受理 |

`{"ok":true}` 仅表示请求已受理，不表示所有传感器已经启流成功。主循环在下一次处理时创建新的 session。

### `stop`

```text
stop\n
```

| 条件 | 响应 | 含义 |
|------|------|------|
| 正在采集 | `{"ok":true,"elapsed_ms":45230}` | 已发出停止信号；`elapsed_ms` 为停止请求时的单调时钟时长 |
| 空闲 | `{"ok":false,"error":"not running"}` | 没有正在进行的 session |

成功响应表示停止信号已设置；视频收尾、编码器 flush 和子进程退出仍会在 session 清理流程中完成。随后应查询 `status`：主线程会先完成 `run_session()` 的清理再处理下一条 Socket 命令，因此收到 `running:false` 的 `status` 响应时，本次 session 已回到空闲循环。

### `status`

```text
status\n
```

响应结构：

```json
{
  "ok": true,
  "ready": true,
  "running": false,
  "session": null,
  "elapsed_ms": 0,
  "cameras": {
    "jhh2_left": true,
    "jhh2_right": true,
    "jhh04": true,
    "jhh02": true
  },
  "imu": true,
  "as5600": false,
  "vive": true
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `ok` | boolean | 命令是否已被处理 |
| `ready` | boolean | 枚举和外设检测是否完成 |
| `running` | boolean | 是否有正在运行的 session；停止后的 `status:false` 响应表示主线程已完成本次 session 清理 |
| `session` | `null` | 当前实现始终返回 `null`；不要依赖 session 名称 |
| `elapsed_ms` | integer | 运行中的 session 时长；空闲时为 `0` |
| `cameras` | object | 已启用摄像头的可用状态；未检测到的六目通道不会出现在对象中 |
| `imu` | boolean | IMU 功能是否启用 |
| `as5600` | boolean | AS5600 功能是否启用 |
| `vive` | boolean | VIVE Tracker 功能是否启用 |

### `preview:<channel>:<path>` 与 `preview:<path>`

预览导出命令支持两种格式：

**通道格式（device-ui 当前使用）：**

```text
preview:jhh02:/tmp/camera_preview_jhh02.jpg\n
```

`<channel>` ∈ `jhh02` | `jhh04` | `wrist_left` | `wrist_right`；`<path>` 必须是绝对路径（以 `/` 开头）。

**legacy 单路径格式：**

```text
preview:/tmp/preview.jpg\n
```

两者仅在**采集中**可用（`session_running == true`）。成功返回 `{"ok":true}`，仅表示已登记导出请求；JPEG 由采集线程在后续帧中写入指定路径（先写 `<path>.tmp` 再 `rename` 原子落盘），调用方应在读取文件前确认其已生成。

> **关键约束：没有「仅预览不落盘」模式。** `preview` 要求当前有正在运行的 session，而 running session 本身就是一次完整采集（会写 MKV/Y8/IMU）。因此上层（device-ui）的「实时预览」是用「启动一个临时采集 session + 抽帧 + 停止后删除该 session」实现的，不是守护进程的原生预览态。

| 条件 | 响应 |
|------|------|
| 正在采集 | `{"ok":true}` |
| 空闲 | `{"ok":false,"error":"not running"}` |
| 非法通道或非绝对路径 | `{"ok":false,"error":"unknown command"}` |

### 未知命令

```text
reboot\n
```

```json
{"ok":false,"error":"unknown command"}
```

## 调用示例

```bash
SOCK=/tmp/unified_capture.sock

echo 'status' | nc -U "$SOCK"
echo 'start' | nc -U "$SOCK"

# 等待采集真正进入运行状态
while ! echo 'status' | nc -U "$SOCK" | grep -q '"running":true'; do
  sleep 0.1
done

echo 'preview:/tmp/preview.jpg' | nc -U "$SOCK"
echo 'stop' | nc -U "$SOCK"

# 等待收尾完成
while echo 'status' | nc -U "$SOCK" | grep -q '"running":true'; do
  sleep 0.1
done
```

## 前端接入约束

- 每次调用都新建连接、写入一条以换行结尾的命令，然后读取完整的一行 JSON 响应。
- `start` 成功后轮询 `status.running`，不要将受理响应当作采集已就绪。
- `stop` 成功后继续轮询 `status.running`，直到为 `false`；这能覆盖视频封装和文件关闭的收尾时间。
- `preview` 成功仅表示请求已登记，读取 JPEG 前应确认目标文件已经生成。
- 「实时预览」不是守护进程的原生能力：`preview` 只在 `running` 时可用，所以上层要先 `start` 一个临时 session，再抽帧，最后 `stop` 并删除该 session。device-ui 用 `<session_dir>/.device-ui-preview` 标记这种临时 session，停止预览时删除、点「录制」时删掉标记「转正」为正式录制（`promote`）。`status.running` 无法区分「预览 session」和「正式录制 session」，上层必须用该标记自己区分——只看到 `running:true` 就把 UI 置为「录制中」是错的。
- 以 `--single` 启动的守护进程在完成一次 session 后即退出（systemd `Restart=always` 会拉起）。停止预览会触发一次退出+重启，重启的几秒内 socket 不可用，上层轮询要容忍 `unreachable`/超时。

## 相关文件

- [`../app/runtime.cpp`](../app/runtime.cpp) — Socket 生命周期、命令处理与主线程 `poll()` 循环。
- [`../app/socket_server.cpp`](../app/socket_server.cpp) — Socket 建立与文本命令解析。
- [`../tests/test_socket.sh`](../tests/test_socket.sh) — Socket 协议验收脚本。
- [`../deploy/unified_capture.service`](../deploy/unified_capture.service) — systemd 服务定义。

## 变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-25 | 首次定义 Socket 控制接口 |
| 2026-07-27 | 迁入 `docs/`，按单线程 `poll()` 实现更新协议与状态语义 |
| 2026-08-13 | 补充 `preview:<channel>:<path>` 通道格式、预览=临时录制的约束、`.device-ui-preview` 标记与 promote、`--single` 退出语义 |
