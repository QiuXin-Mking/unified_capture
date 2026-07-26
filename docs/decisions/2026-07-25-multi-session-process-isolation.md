# ADR：使用单 Session 进程隔离 TSTC SDK 状态

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已采纳 |
| 决策日期 | 2026-07-25 |
| 最后更新 | 2026-07-26 |
| 决策人 | unified_capture 团队 |
| 影响范围 | 采集 Session 生命周期与 systemd 服务 |
| 取代 | 无 |
| 被取代于 | 无 |

---

## 背景

同一进程连续执行多个录制 Session 时，TSTC SDK v1.0.0 的 `STREAM_STATUS(1)` 会在后续 Session 的 setup 阶段返回异常或永久阻塞。完整执行公开的 CREATE、INIT、DEAL、UNINIT 和 DELETE 生命周期后，问题仍会出现。

## 决策驱动因素

1. 黑盒 SDK 没有源码和完整状态机文档。
2. 阻塞进程对 `SIGTERM` 无响应，必须避免进入不可恢复状态。
3. 数据采集可以接受数秒 Session 间隔，但不能接受随机死锁。
4. 进程退出能由操作系统统一回收 SDK 资源和全局状态。

## 候选方案

| 方案 | 优点 | 缺点 |
|------|------|------|
| `--single` + systemd 自动重启 | 状态隔离明确，现有验证中无后续 Session 死锁 | Session 间存在重启窗口 |
| 保持设备流常开 | 可实现零间隙切换 | 空闲功耗高，四路解码持续占用资源，改动大 |
| 继续修正 SDK API 生命周期 | 理论上保留同进程多 Session | 无源码和状态机文档，无法确认缺失调用 |

## 决策

生产采集使用 `--single`，每个进程只执行一个 Session；Session 结束后进程主动退出，由 systemd 使用 `Restart=always` 重新拉起下一实例。

## 理由

进程边界是当前唯一能够可靠重置黑盒 SDK 状态的隔离边界。它以可预测的数秒间隔换取确定性，符合采集场景对稳定性高于零间隙切换的优先级。

## 实施约束

- systemd 配置 `Restart=always`，重启间隔由部署配置控制。
- Session 编号通过扫描已有目录递增，进程重启后不得覆盖数据。
- 保留完整 SDK 清理调用链，便于非 `--single` 调试。
- Socket 客户端必须容忍进程重启期间的短暂不可达。

## 正面后果

- 每个 Session 都从新的进程和 SDK 状态开始。
- 避免后续 Session 进入不可响应的 `STREAM_STATUS(1)` 死锁。
- 不依赖未公开的 SDK reset API。

## 负面后果

- Session 之间存在约 2–5 秒不可用窗口，具体取决于服务配置。
- 无法实现零间隙连续录像。
- 上层控制和 UI 必须处理 Socket 暂时不可达。

## 风险

| 风险 | 触发信号 | 缓解措施 |
|------|----------|----------|
| systemd 未重新拉起进程 | Session 后 Socket 长时间不存在 | 服务健康检查和重启日志 |
| Session 编号重复 | 新进程写入已有目录 | 启动时扫描现有目录生成下一编号 |
| 对 SDK 根因作过度结论 | 缺少供应商或源码证据 | 将内部状态未清零保持为推测 |

## 回退方案

若供应商提供已验证的多 Session 调用序列或 reset API，可新建 ADR 取代本决策，在三次以上连续 Session 和满配压力测试通过后移除 `--single`。

## 验证方式

| 验证项 | 操作或指标 | 通过标准 | 状态 |
|--------|------------|----------|------|
| 单 Session | 每个进程执行一次采集后退出 | 当前 Session 正常完成 | 已通过 |
| 自动重启 | 观察 systemd 在退出后拉起服务 | Socket 恢复且可开始下一 Session | 已通过 |
| 数据连续编号 | 连续启动多个进程 | Session 目录不覆盖 | 已通过 |

## 相关记录

- [TSTC SDK 多 Session Bug](../bugs/2026-07-26-tstc-sdk-multi-session-issues.md)
- [device-ui 集成记录](../records/2026-07-26-unified-capture-device-ui-integration-record.md)

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-25 | unified_capture 团队 | 采纳 `--single` 与 systemd 重启方案 |
| 2026-07-26 | Codex | 从编号文件迁移为日期 ADR，并校正根因置信度 |
