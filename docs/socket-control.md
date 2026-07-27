# unified_capture — Socket 控制协议

> Unix Domain Socket 控制接口参考
>
> 本文以当前 `main.cpp` 实现为准。

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

### `preview:<path>`

```text
preview:/tmp/preview.jpg\n
```

仅在采集中可用。成功时返回 `{"ok":true}`，表示已登记导出请求；JPEG 由采集线程在后续帧中写到指定路径，调用方应在读取文件前确认其已生成。

| 条件 | 响应 |
|------|------|
| 正在采集 | `{"ok":true}` |
| 空闲 | `{"ok":false,"error":"not running"}` |

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

## 相关文件

- [`../main.cpp`](../main.cpp) — Socket 建立、命令处理与主线程 `poll()` 循环。
- [`../tests/test_socket.sh`](../tests/test_socket.sh) — Socket 协议验收脚本。
- [`../unified_capture.service`](../unified_capture.service) — systemd 服务定义。

## 变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-25 | 首次定义 Socket 控制接口 |
| 2026-07-27 | 迁入 `docs/`，按单线程 `poll()` 实现更新协议与状态语义 |
