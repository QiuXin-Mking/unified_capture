# unified_capture — Socket 控制协议

> 与前端 UI 解耦的 Unix Domain Socket 控制接口设计文档
> 日期：2026-07-25

## 1. 架构概述

```
┌──────────────┐    Unix Socket     ┌──────────────────────┐
│  前端 UI      │◄─────────────────►│  unified_capture      │
│  (device-ui)  │  /tmp/unified_    │                      │
│              │  capture.sock     │  主线程 (唯一 owner)   │
└──────────────┘                    │    ├─ 空闲: 等 start  │
                                    │    ├─ 采集: run_session│
                                    │    └─ GPIO / Socket   │
                                    │                        │
                                    │  Socket 线程 (薄层)    │
                                    │    └─ accept → cmd →  │
                                    │       原子变量/响应    │
                                    └──────────────────────┘
```

### 核心原则

- **薄层设计**：Socket 线程只操作原子变量 + 返回响应，不直接调 `run_session()`
- **主线程唯一 owner**：所有传感器生命周期在主线程上，避免并发问题
- **GPIO 和 Socket 共存平等**：谁先到谁生效，互不阻塞

## 2. Socket 连接规范

| 项目 | 值 |
|------|-----|
| 路径 | `/tmp/unified_capture.sock` |
| 类型 | `AF_UNIX` + `SOCK_STREAM` |
| 连接模式 | 短连接 — 一个连接只处理一个命令，响应后关闭 |
| 命令分隔 | 换行符 `\n` |
| 响应格式 | 一行 JSON，以 `\n` 结尾 |
| 残留处理 | 启动时先 `connect()` 试探，失败则 `unlink()` 旧文件 |
| 退出清理 | `atexit()` + SIGINT/SIGTERM handler 中 `unlink()` |
| 线程模型 | `pthread_create` / `pthread_join` |

## 3. 命令协议

### 3.1 `start` — 开始采集

```
请求:  start\n
```

| 前置状态 | 响应 | 说明 |
|----------|------|------|
| 空闲 | `{"ok":true,"session":"session_003"}` | 同步等待，5s 超时 |
| 采集中 | `{"ok":false,"error":"already running","session":"session_003"}` | 拒绝重复启动 |
| 初始化中 | `{"ok":false,"error":"not ready"}` | 设备扫描未完成 |
| 超时 | `{"ok":false,"error":"start timeout"}` | 5s 内传感器未成功启流 |

行为：Socket 线程设 `g_socket_start_request = true`，自旋等待 `g_session_running == true`（最多 5s），返回结果。

### 3.2 `stop` — 停止采集

```
请求:  stop\n
```

| 前置状态 | 响应 | 说明 |
|----------|------|------|
| 采集中 | `{"ok":true,"elapsed_ms":45230}` | 记录已停止 |
| 空闲 | `{"ok":false,"error":"not running"}` | 没有正在进行的 session |

行为：Socket 线程设 `g_session_running = false`，立即返回。主线程在 50ms 内检测到并走 cleanup 流程。

### 3.3 `status` — 查询状态

```
请求:  status\n
```

响应示例（空闲，已就绪）：
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

响应示例（采集中）：
```json
{
  "ok": true,
  "ready": true,
  "running": true,
  "session": "session_003",
  "elapsed_ms": 45230,
  "cameras": {
    "jhh2_left": true,
    "jhh2_right": true,
    "jhh04": true,
    "jhh02": false
  },
  "imu": true,
  "as5600": true,
  "vive": true
}
```

响应示例（初始化中）：
```json
{
  "ok": true,
  "ready": false,
  "running": false,
  "session": null,
  "elapsed_ms": 0,
  "cameras": {},
  "imu": false,
  "as5600": false,
  "vive": false
}
```

**字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `ok` | bool | 命令是否成功处理 |
| `ready` | bool | 设备扫描是否完成（false 时 cameras 字段不可信） |
| `running` | bool | 是否正在采集 |
| `session` | string\|null | 当前 session 名称，如 `"session_003"` |
| `elapsed_ms` | int | 当前 session 已采集时长（毫秒），空闲时为 0 |
| `cameras` | object | 各摄像头是否可用（`CAMS[].enabled` + `g_sixcam.enabled`） |
| `imu` | bool | IMU 解码是否可用 |
| `as5600` | bool | AS5600 磁编码器是否探测成功 |
| `vive` | bool | VIVE Tracker 3.0 USB 设备是否存在 |

### 3.4 未知命令

```
请求:  reboot\n
响应:  {"ok":false,"error":"unknown command"}
```

## 4. GPIO 与 Socket 交互语义

| 场景 | GPIO 按钮 | Socket 命令 |
|------|----------|------------|
| 空闲 → 启动 | **按下 = 开始新 session**（toggle） | `start` = 开始新 session |
| 采集中 → 停止 | **按下 = 停止**（toggle） | `stop` = 停止 |
| 采集中再按/再发 start | **忽略**（已是采集中） | 返回 `already running` |
| 空闲时再按/再发 stop | **按了没反应**（toggle 回去=无状态变化） | 返回 `not running` |
| 同时到达 | 主线程单线程处理，先到先得，另一个等到下一轮 | 同左 |

## 5. 初始化与生命周期

### 5.1 启动顺序

```
1. signal() handlers
2. CLI 参数解析
3. socket_init()         ← 创建 /tmp/unified_capture.sock，启动 socket 线程
4. resolve_camera_devices()  ← VID/PID 扫描
5. probe_peripherals()   ← AS5600 I2C 探测 + VIVE USB 检测
6. g_ready = true        ← status 开始返回可信的设备状态
7. gpio_init()           ← GPIO 可能降级到 no-gpio
8. 主循环                 ← 等 start 信号
```

关键：Socket 在设备扫描之前就绪，前端连上后通过 `ready` 字段判断设备是否扫描完毕。

### 5.2 退出流程

```
1. SIGINT/SIGTERM → g_running = false
2. shutdown(sock_fd, SHUT_RDWR) → accept() 返回错误 → socket 线程检查 g_running → 退出
3. pthread_join(socket_thread)
4. unlink("/tmp/unified_capture.sock")
5. gpiod_line_release / gpiod_chip_close
6. exit(0)
```

### 5.3 `--no-gpio` 模式

保持现有逻辑不变：启动即开始采集，只跑一个 session，Ctrl-C 退出。Socket 在此模式下仍然可用，可以用 `stop` 命令提前停止采集。

## 6. 实现要点

### 6.1 新增全局变量

```c
// Socket 控制
static std::atomic<bool> g_socket_start_request{false};  // socket → 主线程: 请求启动
static std::atomic<bool> g_ready{false};                  // 设备扫描是否完成
static bool g_as5600_ok = false;                          // AS5600 探测结果
static bool g_vive_ok = false;                            // VIVE USB 探测结果
static int g_sock_fd = -1;                                // socket listen fd（用于 shutdown）
```

### 6.2 主循环改造点

```c
// main() — 空闲等待 (原第 522-529 行)
struct timespec timeout = {0, 200000000};  // 200ms
int ret = gpiod_line_event_wait(btn, &timeout);
if (ret > 0) {
    // GPIO 触发 (现有逻辑)
    gpiod_line_event_read(btn, &ev);
    if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
        do_start = true;
}
if (g_socket_start_request.exchange(false))
    do_start = true;  // Socket 触发

// run_session() — 采集中等停止 (原第 395-408 行)
while (g_session_running) {
    if (btn) {
        struct timespec to = {0, 50000000};  // 50ms
        gpiod_line_event_wait(btn, &to);  // 原有逻辑
        // ...
    } else {
        usleep(50000);
    }
    // g_session_running 被 socket 线程设为 false 后，while 条件自动退出
}
```

### 6.3 Socket 线程伪代码

```c
static void* socket_thread(void*) {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    // bind + listen ...

    while (g_running) {
        int client_fd = accept(listen_fd, ...);
        if (client_fd < 0) { if (!g_running) break; continue; }

        char buf[256];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        buf[n] = '\0';

        std::string response;
        if (strncmp(buf, "start", 5) == 0)       response = handle_start();
        else if (strncmp(buf, "stop", 4) == 0)   response = handle_stop();
        else if (strncmp(buf, "status", 6) == 0) response = handle_status();
        else response = R"({"ok":false,"error":"unknown command"})";

        write(client_fd, response.c_str(), response.size());
        close(client_fd);
    }
    close(listen_fd);
    return nullptr;
}
```

### 6.4 VIVE USB 探测

检查 sysfs 中是否有 VIVE Tracker 的 USB 设备（VID=0x0bb4, PID=0x030e 或类似），不需要初始化 libsurvive。

### 6.5 AS5600 探测

```c
int fd = open("/dev/i2c-6", O_RDWR);
if (fd >= 0) {
    // ioctl I2C_SLAVE 0x36, 尝试读寄存器 0x0B (状态寄存器)
    // 成功 → g_as5600_ok = true
    close(fd);
}
```

## 7. 前端接入示例

```javascript
// Node.js
const net = require('net');

function unifiedCtl(cmd) {
  return new Promise((resolve, reject) => {
    const c = net.connect('/tmp/unified_capture.sock');
    let buf = '';
    c.on('connect', () => c.write(cmd + '\n'));
    c.on('data', (d) => { buf += d.toString(); });
    c.on('end', () => {
      try { resolve(JSON.parse(buf.trim())); }
      catch (e) { reject(e); }
    });
    c.on('error', reject);
    c.setTimeout(6000, () => { c.destroy(); reject(new Error('timeout')); });
  });
}

// 用法
const status = await unifiedCtl('status');
if (!status.ready) { /* 显示 "正在初始化..." */ }
if (!status.running) { await unifiedCtl('start'); }
// ...
await unifiedCtl('stop');
```

## 8. 变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-25 | 初始版本，19 项设计决策确认 |
