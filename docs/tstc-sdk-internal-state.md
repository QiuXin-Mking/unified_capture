# TSTC SDK 内部状态与多 Session 稳定性

## 现象

同一进程内连续跑多个 session（start → stop → start → stop → ...），第二轮或第三轮 setup 时 `TST_USBCam_Video_STREAM_STATUS(1)` 随机死锁，SIGTERM 无法杀死进程，只能 `kill -9`。

## 实验过程

| 实验 | 条件 | 结果 |
|------|------|------|
| 1 | 正常 SDK 清理（UNINIT + DELETE），无 --single | 2 轮后 STREAM_STATUS 死锁 |
| 2 | 注释掉 SDK 清理，无 --single | 2 轮后仍死锁 |
| 3 | 正常 SDK 清理 + --single | 10 轮全部正常 |
| 4 | 补充 `STREAM_STATUS(0)` 配对调用，无 --single | 2 轮正常，第 3 轮死锁 |
| 5 | 注释清理 + `STREAM_STATUS(0)` 配对，无 --single | 2 轮正常，第 3 轮死锁 |

## SDK API 分析

完整 API 签名见 `USBCam_API.h`，关键接口：

```
CREATE_DEVICE_POINT(dev_info)  → handle       // 创建设备节点（不打开设备）
open(device_path)              → fd           // 手动打开 V4L2 设备
DEAL_WITH_INIT(handle, fd)     → 0/-1         // 初始化（配置设备文件）
DEAL_WITH(handle, fmt)         → 0/-1         // 完整管线（线程内阻塞）：
                                              //   创建缓存→枚举配置→打开流→循环读取→关闭流→清空缓存
STREAM_STATUS(handle, block)   → resource_cnt // 等待/查询数据流状态
EVENT_LoopMode(handle, val)    → void         // 控制 DEAL_WITH 循环次数
GET_FRAME_BUFF(handle, block)  → Frame*       // 获取帧（另一线程调用）
SAVE_FRAME_RES(handle, frame)  → void         // 回收帧资源
DEAL_WITH_UNINIT(handle)       → void         // 释放控制节点、关闭设备文件
DELETE_DEVICE_POINT(handle)    → void         // 删除设备节点
```

### 我们的调用序列

```
Setup:
  CREATE_DEVICE_POINT → open() → DEAL_WITH_INIT → pthread_create(DEAL_WITH)
  → usleep(200ms) → STREAM_STATUS(1)  // 阻塞等待流启动

Collect:
  GET_FRAME_BUFF → SAVE_FRAME_RES 循环

Teardown:
  EVENT_LoopMode(0) → STREAM_STATUS(0) → pthread_join
  → DEAL_WITH_UNINIT → DELETE_DEVICE_POINT
```

### 可疑点排查

1. **缺少 `STREAM_STATUS(0)` 配对**（实验 4）
   - 在 teardown 补充 `STREAM_STATUS(0)` 后，Session 2 不死锁了（从 2 轮变 3 轮才死锁）
   - 说明 `STREAM_STATUS(1)/(0)` 配对**部分有效**，但不能完全解决问题

2. **`DEAL_WITH_UNINIT` 不关 fd**
   - 文档说"关闭设备文件"，但内部实现可能不保证
   - 加显式 `close()` 无效

3. **VID/PID 冲突**
   - jhh2_left、jhh2_right、jhh02 共享 VID=1bcf PID=2d50，共 3 个设备
   - 但首轮 session 正常，说明 SDK 能区分同 VID/PID 的不同设备

4. **全局静态状态**
   - SDK 只有头文件、无源码，无法确认内部实现
   - 行为表现符合"全局静态表 + 引用计数泄漏"模型

## 结论

**TSTC SDK (v1.0.0, 2022-03-24) 不支持同进程内多次完整设备生命周期。** 内部存在无法通过公开 API 完全重置的全局状态。无论是否调用清理函数、是否补充 `STREAM_STATUS(0)` 配对，第二轮或第三轮 session 必然死锁。

SDK 设计上可能预期设备节点创建后长期持有，而非频繁创建/销毁。

## 解决方案

**`--single` 模式 + systemd `Restart=always`：**

- 每次 session 结束后进程主动退出
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

- `main.cpp` — `--single` 在 socket 模式触发 `break`；session 编号递增
- `video_sensor.h` — SDK 完整生命周期（CREATE → INIT → DEAL_WITH → UNINIT → DELETE）
- `sixcam_sensor.h` — 同上，双通道串行初始化
- `unified_capture.service` — systemd 配置
- `USBCam_API.h` — SDK API 头文件（`/usr/local/TSTC/include/USBCam_API/`）
