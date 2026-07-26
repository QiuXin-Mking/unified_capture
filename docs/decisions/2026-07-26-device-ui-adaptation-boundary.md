# ADR：由 device-ui 后端承担采集协议适配

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已采纳 |
| 决策日期 | 2026-07-26 |
| 最后更新 | 2026-07-26 |
| 决策人 | unified_capture / device-ui 团队 |
| 影响范围 | device-ui 前后端边界与 unified_capture 对接 |
| 取代 | 无 |
| 被取代于 | 无 |

---

## 背景

device-ui 已通过 `deviceApi.ts` 使用稳定的录制、状态、预览和文件接口。unified_capture 使用 Unix Socket 和新的 Session 目录结构。对接可以修改前端契约、修改 unified_capture 模拟旧协议，或在 device-ui 后端建立适配层。

## 决策驱动因素

1. 5.5 英寸触摸屏上的前端现场调试成本高。
2. 前端 API 已集中在 `deviceApi.ts`，现有语义能够承载新状态。
3. 必须继续兼容旧 stereo daemon 与 guidaview 链路。
4. 协议、文件系统和进程检测属于后端职责。

## 候选方案

| 方案 | 优点 | 缺点 |
|------|------|------|
| 修改前端直接理解 unified_capture | 能暴露全部新能力 | 扩大 UI 改动面，破坏旧接口兼容 |
| 修改 unified_capture 模拟旧 daemon | 前端和 Node 后端改动少 | 让采集进程承担 UI 历史协议 |
| 在 `server.cjs` 添加适配层 | 前端契约稳定，可集中兼容新旧后端 | `server.cjs` 增加分支和映射逻辑 |

## 决策

所有 unified_capture 协议、状态和文件结构适配放在 device-ui 的 `server.cjs`；前端 `deviceApi.ts` 保持既有接口语义，仅以可选字段扩展新传感器状态。

## 理由

Node 后端天然位于设备协议和 UI 模型之间，适合吸收 Unix Socket、进程模式和目录结构差异。保持前端契约稳定可以缩小触摸屏端回归范围，并保留旧采集链路。

## 实施约束

- `server.cjs` 封装 `captureCtl()` 与 `captureActive()`。
- 新分支优先处理 unified_capture，未检测到时保留旧链路。
- 前端新增字段使用可选属性，旧响应仍能解析。
- 不向前端透传不必要的底层 Socket 错误细节。
- 录制 toggle 使用忙标志串行化，避免并发启停。

## 正面后果

- 前端页面和主要 API 调用方式保持兼容。
- 新旧采集后端可由同一 server API 暴露。
- 设备协议变化集中在一个适配边界内。

## 负面后果

- `server.cjs` 需要维护两套后端分支。
- 新能力受现有前端模型约束，专属功能需要后续扩展。
- 适配层测试需要覆盖两种运行模式。

## 风险

| 风险 | 触发信号 | 缓解措施 |
|------|----------|----------|
| 新分支改变旧链路行为 | 未运行 unified_capture 时旧 API 回归失败 | 保留旧代码并执行 fallback 回归 |
| 可选字段语义不一致 | UI 显示与实际传感器状态不符 | 在后端集中定义映射并添加 API 实机验证 |
| `server.cjs` 分支持续膨胀 | 新后端继续增加 | 当第三种后端出现时抽取独立 adapter 模块 |

## 回退方案

若后端适配无法保持清晰边界，可回退到专用 API 前缀并由前端显式选择运行模式；回退前保留旧 API，避免一次性破坏兼容性。

## 验证方式

| 验证项 | 操作或指标 | 通过标准 | 状态 |
|--------|------------|----------|------|
| 录制启停 | 调用现有 toggle API | unified_capture 正确 start/stop | 已通过 |
| 状态接口 | 调用现有 record/status API | UI 所需字段正确映射 | 已通过 |
| 旧链路兼容 | unified_capture 不可达时运行旧模式 | 进入原有 stereo/guidaview 分支 | 代码保留，回归待补充 |

## 相关记录

- [device-ui 集成记录](../records/2026-07-26-unified-capture-device-ui-integration-record.md)
- [集成实施计划](../plans/2026-07-26-integrate-unified-capture-with-device-ui.md)

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-26 | unified_capture / device-ui 团队 | 在集成实现中采用后端适配边界 |
| 2026-07-26 | Codex | 从集成记录提取为 ADR |
