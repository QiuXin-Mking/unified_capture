# BUG: pthread_create socket 线程导致 TSTC/MPP 全线崩溃

> 日期: 2026-07-25 | 严重级别: Critical

## 现象

在 `main.cpp` 中新增 Unix Domain Socket 控制功能后:
- **MPP 硬件编码器**: `mpp_buffer_group_init:618` 断言失败, 所有 encoder 无法分配 DMA buffer
- **TSTC SDK**: 第 3 个 JHH2 设备的 `STREAM_STATUS(1)` 永久阻塞
- **数据产出**: 全部文件 0 字节

## 根因

```cpp
// 罪魁祸首: 在设备初始化之前或之后创建额外线程
pthread_create(&g_socket_thread, nullptr, socket_thread, nullptr);
```

一个在后台运行 `accept()` 的 **pthread** 会干扰 RK3588 平台上的 TSTC SDK 和 MPP 驱动初始化。  
即使线程只做 socket I/O, 不碰任何硬件, 仍会导致:

1. **MPP DMA buffer 分配失败** (`mpp_buffer_group_init:618` 断言)
2. **TSTC SDK 流启动阻塞** (第 3 个同 VID/PID 设备的 `STREAM_STATUS` 死锁)

## 排查过程

1. git worktree 切到 socket 改动前的 commit (`b98fa54`), 旧版 4 路全正常 → **硬件没问题**
2. 逐步回退 socket 相关的每一个改动, 最终定位到 `pthread_create` 这一行
3. 注释掉 `socket_init()` → 一切正常; 加回来 → 全线崩溃
4. 已验证不是 systemd、probe_peripherals、camera matching 的问题

## 解决方案

**放弃 socket 线程, 改用单线程 poll 模式**:

```cpp
// 主线程创建非阻塞 listen socket
g_sock_fd = socket_setup();  // socket() + bind() + listen() + O_NONBLOCK

// 主循环: poll 同时监听 GPIO 和 socket fd
while (g_running) {
    struct pollfd pfds[2];
    pfds[0].fd = gpiod_line_event_get_fd(btn);
    pfds[1].fd = g_sock_fd;
    poll(pfds, 2, 200);

    // 处理 socket (非阻塞 accept)
    if (pfds[1].revents & POLLIN) {
        int c = accept4(g_sock_fd, ..., SOCK_NONBLOCK);
        socket_handle_client(c);
    }

    // 检查 start 请求
    if (g_socket_start_request.exchange(false))
        启动 session;
}
```

`run_session()` 内部的等停循环也用同样的 poll 方式监听 socket。

## 经验教训

1. **RK3588 + TSTC SDK + MPP 的组合对额外线程极其敏感** — 即使线程什么都不做, 只要有 `pthread_create` 在设备初始化附近, 就可能破坏驱动状态
2. 排查这类"加了代码就不行"的问题时, **二分回退法** (git worktree + 逐步注释) 是最有效的手段
3. 涉及硬件 SDK 的平台, **优先用单线程 poll 代替多线程**, 避免引入不可预测的线程调度副作用

## 相关文件

- `main.cpp` — 最终版本使用 poll() 而非 pthread
- `main_v2.cpp` — 旧版 (pthread, 失败)
- `main_v3.cpp` — 无线程版原型 (成功)
