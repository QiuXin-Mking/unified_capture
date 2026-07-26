# BUG：Socket pthread 导致 TSTC/MPP 全线崩溃

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已规避 |
| 严重级别 | Critical |
| 首次发现 | 2026-07-25 |
| 最后更新 | 2026-07-26 |
| 负责人 | unified_capture 团队 |
| 影响版本 | 引入 Socket 控制线程后的版本 |
| 关联任务 | 回退对照提交 `b98fa54` |

> **目标：** 记录额外 Socket pthread 与 RK3588 上 TSTC SDK、MPP 初始化异常之间的复现证据及规避方案。

---

## 现象

在 `main.cpp` 中新增 Unix Domain Socket 控制线程后：

- MPP 硬件编码器触发 `mpp_buffer_group_init:618` 断言，所有编码器无法分配 DMA buffer。
- 第 3 个 JHH2 设备在 TSTC SDK 的 `STREAM_STATUS(1)` 永久阻塞。
- 所有采集输出文件均为 0 字节。

## 影响

- 影响功能：全部摄像头采集、H.265 编码和控制流程。
- 影响设备或数据：全部采集文件为 0 字节。
- 是否阻塞发布或交付：是。
- 临时恢复方式：移除额外 pthread，改为主线程单线程 `poll()`。

## 环境

| 项目 | 值 |
|------|----|
| 硬件 | RK3588 |
| 操作系统 / 内核 | 原记录未注明 |
| 软件提交 | Socket 控制功能引入后的版本；对照提交 `b98fa54` |
| SDK / 驱动 | TSTC SDK、Rockchip MPP |
| 设备连接 | 多路 JHH2；第 3 个同 VID/PID 设备可触发阻塞 |
| 启动参数 | 原记录未注明 |

## 复现步骤

### Step 1：创建后台 Socket 线程

**操作：**

```cpp
pthread_create(&g_socket_thread, nullptr, socket_thread, nullptr);
```

线程在后台执行阻塞式 `accept()`，不访问摄像头或编码器。

**预期：** Socket 控制线程与摄像头、MPP 初始化互不影响。

### Step 2：启动多路采集

**操作：** 按原采集流程初始化 TSTC 设备和 MPP 编码器。

**预期：** 所有设备开始采集，编码器成功分配 DMA buffer。

### Step 3：移除线程进行对照

**操作：** 注释 `socket_init()` 或切换到 Socket 改动前的提交 `b98fa54`，以相同硬件重新运行。

**预期：** 若故障与额外线程相关，移除线程后恢复正常。

## 预期结果

新增只处理 Socket I/O 的线程不应改变 TSTC 设备初始化、MPP DMA buffer 分配或数据写入结果。

## 实际结果

存在 Socket pthread 时，MPP 初始化断言失败、第 3 个 TSTC 设备流启动阻塞，并产生 0 字节文件；移除该线程后四路采集恢复正常。

## 证据

### 关键日志

```text
mpp_buffer_group_init:618
STREAM_STATUS(1)
```

原记录未保存更完整的日志上下文。

### 实验结果

| 条件 | 结果 | 结论 |
|------|------|------|
| Socket 改动前提交 `b98fa54` | 旧版四路正常 | 硬件本身可正常工作 |
| 启用 `socket_init()` 和后台 pthread | TSTC/MPP 全线异常 | 故障与 Socket 线程同时出现 |
| 注释 `socket_init()` | 采集恢复正常 | 移除线程可稳定规避 |
| 逐步回退 Socket 相关改动 | 定位到 `pthread_create` | 额外 pthread 是最小触发改动 |

## 根因分析

**结论状态：** 推测

现有对照实验确认 `pthread_create` 是最小触发条件，但尚无驱动或 SDK 内部证据解释线程为何影响 DMA 分配和流启动。因此可以确认“额外 pthread 会触发故障”，不能把“TSTC/MPP 内部线程调度或全局状态损坏”写成已确认根因。

如需确认底层根因，应补充 TSTC SDK 调试符号、MPP 驱动日志以及线程创建前后的系统调用跟踪。

## 解决或规避方案

### 当前方案

放弃 Socket 线程，使用主线程单线程 `poll()` 同时监听 GPIO 和非阻塞 Socket：

```cpp
g_sock_fd = socket_setup();  // socket + bind + listen + O_NONBLOCK

while (g_running) {
    struct pollfd pfds[2];
    pfds[0].fd = gpiod_line_event_get_fd(btn);
    pfds[1].fd = g_sock_fd;
    poll(pfds, 2, 200);

    if (pfds[1].revents & POLLIN) {
        int c = accept4(g_sock_fd, nullptr, nullptr, SOCK_NONBLOCK);
        socket_handle_client(c);
    }

    if (g_socket_start_request.exchange(false)) {
        // 启动 session
    }
}
```

`run_session()` 的等待循环采用同样方式监听 Socket。

### 风险与限制

- 当前方案是工程规避，未解释 TSTC SDK 或 MPP 的底层根因。
- 后续重新引入任何后台线程时，需要在目标板上执行完整多路采集回归。

## 验证结果

**验证状态：** 已通过

| 验证项 | 操作 | 预期 | 实际 | 结论 |
|--------|------|------|------|------|
| 旧版对照 | 切换到 `b98fa54` 运行四路采集 | 四路正常 | 四路正常 | 通过 |
| 移除 Socket 线程 | 注释 `socket_init()` 后运行 | TSTC/MPP 恢复 | 一切正常 | 通过 |
| 单线程方案 | 使用 `poll()` 处理 Socket | 控制功能保留且采集正常 | 原记录标记成功 | 通过 |

## 相关文件

- `main.cpp` — 最终使用 `poll()` 的控制与采集主循环。
- `main_v2.cpp` — 原记录中的 pthread 失败版本；当前仓库是否保留待确认。
- `main_v3.cpp` — 原记录中的无线程成功原型；当前仓库是否保留待确认。

## 经验教训

1. 在 RK3588、TSTC SDK 与 MPP 的组合环境中，引入额外线程后必须执行完整硬件回归。
2. “增加无关代码后硬件链路异常”可使用 Git 对照版本与逐步注释定位最小触发改动。
3. 控制事件数量有限时，单线程 `poll()` 可减少硬件 SDK 外部的调度变量。
4. 区分“触发条件已确认”和“底层根因已确认”，避免证据越界。

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-25 | unified_capture 团队 | 首次记录故障、排查过程和单线程规避方案 |
| 2026-07-26 | Codex | 迁入统一 Bug 目录并按标准结构整理 |
