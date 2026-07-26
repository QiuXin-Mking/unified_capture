# 架构决策记录

本目录保存 Architecture Decision Record（ADR），用于说明重要技术选择的背景、候选方案、取舍和后果。执行过程与测试结果放在 [`../records/`](../records/)，故障证据放在 [`../bugs/`](../bugs/)。

## 使用方式

1. 搜索是否已有相同决策。
2. 复制 `TEMPLATE.md`，使用 `YYYY-MM-DD-<slug>.md` 命名。
3. 每份 ADR 只记录一个可独立撤销或替换的决策。
4. 参数与实现细节写入所属 ADR 的“实施约束”。
5. 链接来源 record 和相关 Bug。
6. 执行 `AGENTS.md` 中的检查。

## 状态

| 状态 | 含义 |
|------|------|
| 提议 | 尚未批准 |
| 已采纳 | 当前有效并应遵循 |
| 已弃用 | 不建议用于新实现，但没有明确替代 ADR |
| 已取代 | 已被另一份 ADR 替换 |
| 已拒绝 | 经过评估但未采用 |

## ADR 索引

| 日期 | 决策 | 状态 |
|------|------|------|
| 2026-07-23 | [摄像头按阶段接入并设置验证门禁](2026-07-23-staged-camera-validation.md) | 已采纳 |
| 2026-07-25 | [使用单 Session 进程隔离 TSTC SDK 状态](2026-07-25-multi-session-process-isolation.md) | 已采纳 |
| 2026-07-26 | [在采集循环内同步生成预览 JPEG](2026-07-26-preview-jpeg-in-collect-loop.md) | 已采纳 |
| 2026-07-26 | [由 device-ui 后端承担采集协议适配](2026-07-26-device-ui-adaptation-boundary.md) | 已采纳 |
| 2026-07-26 | [自动检测运行模式并短期缓存](2026-07-26-runtime-mode-auto-detection.md) | 已采纳 |
| 2026-07-26 | [将 session 目录映射为 Recording 模型](2026-07-26-session-recording-mapping.md) | 已采纳 |
