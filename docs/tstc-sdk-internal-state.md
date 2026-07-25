# TSTC SDK 内部状态与多 Session 稳定性

## 现象

同一进程内连续跑多个 session（start → stop → start → stop → ...），第二轮 setup 时 `TST_USBCam_Video_STREAM_STATUS(1)` 随机死锁，SIGTERM 无法杀死进程，只能 `kill -9`。

## 实验过程

| 实验 | 条件 | 结果 |
|------|------|------|
| 1 | 正常 SDK 清理（UNINIT + DELETE），无 --single | 2 轮后 STREAM_STATUS 死锁 |
| 2 | 注释掉 SDK 清理，无 --single | 2 轮后仍死锁 |
| 3 | 正常 SDK 清理 + --single | 10 轮全部正常 |

## 结论

**TSTC SDK 内部维护全局静态状态表。** `CREATE_DEVICE_POINT` 向内部表注册设备记录，`DELETE_DEVICE_POINT` 移除。同进程内连续创建/销毁设备点时：

1. SDK 内部状态累积或索引冲突
2. 第二轮起的 `STREAM_STATUS(1)` 在 SDK 内部阻塞等待某个永远不会到达的条件
3. 无论是否调用 SDK 清理函数，问题都会出现

注释掉 `DEAL_WITH_UNINIT` / `DELETE_DEVICE_POINT` **不能**解决问题——不调用清理意味着旧设备记录永远残留在 SDK 内部表中，第二轮起直接冲突。

## 解决方案

**`--single` 模式 + systemd `Restart=always`：**

- 每次 session 结束后进程主动退出（`--single` 触发 `break`，`main()` 正常返回）
- systemd 在 5 秒内重新拉起新进程
- 新进程 = 全新地址空间 = SDK 内部状态归零
- Session 编号通过扫描已有目录递增，数据不覆盖

```ini
# unified_capture.service
ExecStart=/usr/local/bin/unified_capture --socket --no-vive --single /data/capture
Restart=always
RestartSec=5
```

## 代价

- 每次 session 之间有 3-5 秒进程重启窗口（socket 不可用）
- session 之间共享的状态（如果有）需要落盘

## 相关文件

- `main.cpp` — `--single` 在 socket 模式触发 `break`
- `video_sensor.h` — `stream_thread_func` 中 SDK 清理
- `sixcam_sensor.h` — `teardown()` 中 SDK 清理
- `unified_capture.service` — systemd 配置
