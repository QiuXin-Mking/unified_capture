# BUG：TSTC SDK 多 Session 状态与资源所有权异常

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已规避 |
| 严重级别 | High |
| 首次发现 | 2026-07-26 |
| 最后更新 | 2026-07-26 |
| 负责人 | unified_capture 团队 / TSTC SDK 供应商 |
| 影响版本 | USBCam_API v1.0.0（build: Mar 24 2022 15:37:12） |
| 关联任务 | 供应商问题反馈 |

> **目标：** 记录 TSTC SDK 在同进程多 Session 下的死锁、状态机不明确和文件描述符所有权问题。

---

## 现象

同一进程完整执行两次及以上设备生命周期后，再次调用 `STREAM_STATUS(handle, 1)` 会返回负值或永久阻塞。进程对 `SIGTERM` 无响应，只能使用 `kill -9`。

同时，SDK 文档未说明 `EVENT_LoopMode` 与 `STREAM_STATUS` 的多 Session 配对规则，也未明确 `DEAL_WITH_UNINIT` 是否关闭调用者传入的文件描述符。

## 影响

- 影响功能：同一进程内连续执行多个采集 Session。
- 影响设备或数据：Session 2 或 Session 3 无法开始，连续录像中断。
- 是否阻塞发布或交付：阻塞零间隙连续录像。
- 临时恢复方式：每个 Session 结束后退出进程，由 systemd 重启。

## 环境

| 项目 | 值 |
|------|----|
| 硬件 | RK3588；4 路 USB3 Vision 设备 |
| 操作系统 / 内核 | Linux 5.10.160，aarch64 |
| 软件提交 | 原记录未注明 |
| SDK / 驱动 | USBCam_API v1.0.0（build: Mar 24 2022 15:37:12） |
| 设备连接 | 2× JHH2 独立双目 3840×1200@30；JHH02/JHH04 六目模组 4000×1200 或 3104×480@30 |
| 启动参数 | 原记录未注明 |

## 复现步骤

### Step 1：在同一进程循环创建设备

**操作：**

```cpp
for (int session = 1; session <= 3; session++) {
    void* handle = CREATE_DEVICE_POINT(dev_info);
    int fd = open(dev_info.Device_Path, O_RDWR);
    DEAL_WITH_INIT(handle, fd);

    pthread_create(&thread, nullptr, stream_func, handle);
    usleep(200000);

    STREAM_STATUS(handle, 1);

    // GET_FRAME_BUFF 采集

    EVENT_LoopMode(handle, 0);
    STREAM_STATUS(handle, 0);
    pthread_join(thread, nullptr);
    DEAL_WITH_UNINIT(handle);
    DELETE_DEVICE_POINT(handle);
    close(fd);
}
```

**预期：** 每个 Session 都完成相同的创建、采集和销毁生命周期。

### Step 2：观察后续 Session 的流启动

**操作：** 记录每个 Session 调用 `STREAM_STATUS(handle, 1)` 的返回值和耗时。

**预期：** Session 1 至 Session 3 均在有限时间内成功返回。

### Step 3：切换清理与配对方式

**操作：** 分别测试完整清理、不调用 `UNINIT/DELETE`、补充 `STREAM_STATUS(0)` 三种条件。

**预期：** 正确的清理和配对方式应允许后续 Session 正常启动。

## 预期结果

SDK 应支持同一进程内多次完整设备生命周期，或在文档中明确不支持并提供正确的状态重置方法。所有 API 的调用顺序、配对要求和 fd 所有权应明确。

## 实际结果

- Session 1 始终正常。
- 完整清理时，Session 3 在 `STREAM_STATUS(1)` 死锁。
- 不调用 `UNINIT/DELETE` 时，Session 2 即死锁。
- 补充 `STREAM_STATUS(0)` 后 Session 2 通过，但 Session 3 仍死锁。
- SDK 文档无法回答状态转移和 fd 所有权问题。

## 证据

### 关键日志

```text
STREAM_STATUS(handle, 1)  // Session 2/3 返回负值或永久阻塞
```

进程阻塞后对 `SIGTERM` 无响应，只能 `kill -9`。原记录未提供完整日志。

### 实验结果

| 实验条件 | Session 1 | Session 2 | Session 3 |
|----------|-----------|-----------|-----------|
| 完整 API 调用链（含 UNINIT + DELETE） | 通过 | 通过 | 死锁 |
| 无 UNINIT / DELETE（依赖进程退出回收） | 通过 | 死锁 | 未执行 |
| 补充 `STREAM_STATUS(0)` 配对调用 | 通过 | 通过 | 死锁 |

### 待供应商确认的问题

1. `STREAM_STATUS(handle, 1)` 的返回值是否为引用计数，是否必须与 `STREAM_STATUS(handle, 0)` 配对？
2. `EVENT_LoopMode(handle, 0)` 使 `DEAL_WITH` 返回后，`STREAM_STATUS` 处于什么状态？
3. `DEAL_WITH_UNINIT` 关闭调用者传入的 fd，还是仅关闭 SDK 内部 fd？
4. SDK 是否支持同一进程内多次设备生命周期，是否存在未公开的 reset API？

## 根因分析

**结论状态：** 推测

实验表明 `STREAM_STATUS(0)` 会改变下一次 Session 的结果，SDK 内部可能存在跨 Session 的计数或全局状态；完整清理后 Session 3 仍死锁，说明公开清理链路没有恢复全部相关状态。但缺少 SDK 源码、状态机文档或供应商确认，不能将“全局静态状态表未清零”认定为事实。

## 解决或规避方案

### 当前方案

每次录像仅运行一个 Session，结束后主动退出进程，由 systemd 重新启动：

```ini
[Service]
ExecStart=/path/to/app --socket --single /data/capture
Restart=always
RestartSec=5
```

进程边界用于强制释放 SDK 内部状态。

### 风险与限制

- Session 之间存在 3–5 秒不可用间隔。
- 无法实现零间隙连续录像。
- fd 所有权仍不明确，调用者重复 `close()` 或遗漏 `close()` 的风险待供应商确认。
- 规避方案不等于 SDK 根因修复。

## 验证结果

**验证状态：** 部分通过

| 验证项 | 操作 | 预期 | 实际 | 结论 |
|--------|------|------|------|------|
| 单 Session 采集 | 每个进程只运行一个 Session | 采集正常结束 | 功能正常 | 通过 |
| systemd 重启 | Session 结束后退出并重启 | 下一次采集可启动 | 可恢复，间隔 3–5 秒 | 通过 |
| 同进程三 Session | 完整执行三次生命周期 | 三次均正常 | Session 3 死锁 | 失败 |
| fd 所有权 | 检查 SDK 文档或供应商答复 | 明确唯一关闭方 | 尚无结论 | 未验证 |

## 相关文件

- `main.cpp` — Session 生命周期和进程退出逻辑。
- `unified_capture.service` — systemd 自动重启配置。
- `docs/tstc-sdk-internal-state.md` — SDK 内部状态的独立分析记录。

## 经验教训

1. 第三方硬件 SDK 的“资源释放 API 返回”不代表进程内全局状态已经复位。
2. 多 Session 行为需要以至少三次连续生命周期验证，单次或两次成功不足以证明稳定。
3. API 文档必须明确状态转移、配对调用和资源所有权；缺少这些信息时应将推测与事实分开记录。
4. 进程重启是可验证的隔离手段，但需要明确记录可用性代价。

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-26 | unified_capture 团队 | 首次整理三个 TSTC SDK 问题并形成供应商反馈 |
| 2026-07-26 | Codex | 迁入统一 Bug 目录并按标准结构整理 |
