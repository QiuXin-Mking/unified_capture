# TSTC SDK 问题反馈

**SDK 版本:** USBCam_API v1.0.0 (build: Mar 24 2022 15:37:12)
**平台:** RK3588, Linux 5.10.160, aarch64
**摄像头:** 4 路 USB3 Vision 设备（2× JHH2 独立双目 3840×1200@30 + 六目模组 JHH02/JHH04 4000×1200/3104×480@30）
**日期:** 2026-07-26

---

## 问题 1: STREAM_STATUS(1) 死锁 —— 同进程内多 Session 不支持

### 严重程度: 高

### 现象

同一进程内完整执行两次及以上 `CREATE_DEVICE_POINT → DEAL_WITH_INIT → DEAL_WITH → DEAL_WITH_UNINIT → DELETE_DEVICE_POINT` 生命周期后，再次调用 `STREAM_STATUS(handle, 1)` 时死锁，返回值为负或永不返回。进程对 SIGTERM 无响应，只能 `kill -9`。

### 复现步骤

```c
// 伪代码
for (int session = 1; session <= 3; session++) {
    void* handle = CREATE_DEVICE_POINT(dev_info);
    int fd = open(dev_info.Device_Path, O_RDWR);
    DEAL_WITH_INIT(handle, fd);
    
    pthread_create(&thread, NULL, stream_func, handle);
    usleep(200000);
    
    STREAM_STATUS(handle, 1);  // Session 2/3 在此死锁
    
    // ... GET_FRAME_BUFF 采集 ...
    
    EVENT_LoopMode(handle, 0);
    STREAM_STATUS(handle, 0);
    pthread_join(thread, NULL);
    DEAL_WITH_UNINIT(handle);
    DELETE_DEVICE_POINT(handle);
    close(fd);
}
```

### 实验数据

| 实验条件 | Session 1 | Session 2 | Session 3 |
|----------|-----------|-----------|-----------|
| 完整 API 调用链（含 UNINIT + DELETE） | ✅ | ✅ | ❌ 死锁 |
| 无 UNINIT / DELETE（依赖进程退出回收） | ✅ | ❌ 死锁 | - |
| 补充 `STREAM_STATUS(0)` 配对调用 | ✅ | ✅ | ❌ 死锁 |

所有实验中 Session 1 始终正常。

### 分析

SDK 内部疑似维护全局静态状态表（可能按 VID/PID 索引），`CREATE_DEVICE_POINT` 注册记录，`DELETE_DEVICE_POINT` 移除。但完整调用链后内部状态未能完全归零，导致后续 Session 的 `STREAM_STATUS(1)` 内部等待条件永不满足。

补充 `STREAM_STATUS(0)` 调用后 Session 2 从死锁变为通过，表明 `STREAM_STATUS(1)/(0)` 存在配对计数机制。但 Session 3 仍死锁，说明还有其他未归零的内部状态。

### 期望

- 确认 SDK 是否设计为支持同进程内多次设备生命周期
- 如果是，请提供正确清理内部状态的方法（是否有未文档化的 reset API？）
- 提供 SDK 内部状态机设计文档，特别是 `STREAM_STATUS`、`EVENT_LoopMode`、`DEAL_WITH` 之间的状态转移关系

---

## 问题 2: EVENT_LoopMode 与 STREAM_STATUS 交互不明确

### 严重程度: 中

### 问题

当前头文件文档仅描述了各 API 的单次行为，未说明多 Session 场景下的配对规则和状态转移：

- `STREAM_STATUS(handle, 1)` 文档说"等待数据流启动"，返回"资源数量"。该值是否引用计数？是否需要 `STREAM_STATUS(handle, 0)` 配对释放？
- `EVENT_LoopMode` 文档说 iVal=0 时 DEAL_WITH 处理最后一帧后返回。但 DEAL_WITH 返回后，STREAM_STATUS 的状态如何？
- 上述函数之间是否存在隐含的调用顺序依赖？

### 期望

提供 API 状态转移图或完整的多 Session 使用示例代码。

---

## 问题 3: DEAL_WITH_UNINIT 与文件描述符管理

### 严重程度: 低

### 问题

文档描述 `DEAL_WITH_UNINIT` 为"释放控制节点->关闭设备文件"，但：
- 它关闭的是由 `DEAL_WITH_INIT(handle, fd)` 传入的那个 fd，还是 DEAL_WITH 内部可能额外打开的 fd？
- 如果 DEAL_WITH 内部也打开了设备文件，UNINIT 是否同时关闭二者？

### 期望

确认 fd 所有权模型，明确调用者是否需要自行 close()。

---

## 当前规避方案

上述问题导致我们在应用中采用了 **单 Session 后进程退出 + systemd 自动重启** 的方案：

```ini
# systemd service
ExecStart=/path/to/app --socket --single /data/capture
Restart=always
RestartSec=5
```

每次录像结束后进程主动退出，systemd 重新拉起新进程——用进程边界强制重置 SDK 内部状态。

此方案功能正常但有以下代价：
- 多 Session 之间有 3-5 秒不可用间隔
- 无法实现零间隙连续录像

---

## 联系信息

如有需要可提供完整的复现程序（C++ 最小示例）及板端远程访问。
