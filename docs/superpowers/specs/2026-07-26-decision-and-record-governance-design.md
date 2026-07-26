# 决策与执行记录治理设计

## 目标

为 `docs/decisions/` 和 `docs/records/` 建立与 `docs/bugs/` 同等级的 Codex 目录规范，并从现有执行记录中提取可独立追踪的架构决策。

## 目录职责

### `docs/decisions/`

保存 Architecture Decision Record（ADR），回答“为什么选择这个方案”。每个文件只记录一个可独立撤销或替换的决策，文件名使用 `YYYY-MM-DD-<slug>.md`。

目录包含：

- `AGENTS.md`：Codex 创建、迁移、修改和审查 ADR 时必须遵循的规则。
- `TEMPLATE.md`：ADR 标准结构。
- `README.md`：状态、命名方式和 ADR 索引。

ADR 固定包含：元数据、背景、决策驱动因素、候选方案、决策、理由、实施约束、正面后果、负面后果、风险、回退方案、验证方式、相关记录、变更记录。

### `docs/records/`

保存测试计划、集成过程和执行结果，回答“做了什么、如何做、结果如何”。文件名使用 `YYYY-MM-DD-<slug>.md`。

目录包含：

- `AGENTS.md`：Codex 创建、迁移、修改和审查执行记录时必须遵循的规则。
- `TEMPLATE.md`：执行记录标准结构。
- `README.md`：记录类型、命名方式和索引。

两份现有记录迁入该目录：

- `2026-07-23-stage-test-plan.md`
- `2026-07-26-unified-capture-device-ui-integration-record.md`

原记录保留事实、命令、实验结果和待办事项，并增加所提取 ADR 的链接。

## ADR 清单

1. `2026-07-23-staged-camera-validation.md`
2. `2026-07-25-multi-session-process-isolation.md`
3. `2026-07-26-preview-jpeg-in-collect-loop.md`
4. `2026-07-26-device-ui-adaptation-boundary.md`
5. `2026-07-26-runtime-mode-auto-detection.md`
6. `2026-07-26-session-recording-mapping.md`

其中预览 ADR 合并最近邻缩放、JPEG 参数和原子写入等同一方案内的实施约束，不为每个参数单独创建 ADR。

## 追溯关系

- 每份 ADR 的“相关记录”链接其事实来源。
- 每份执行记录在顶部列出从中提取的 ADR。
- `README.md` 提供目录内完整索引。
- Bug 仍保留在 `docs/bugs/`，ADR 可引用 Bug，但不复制故障报告。

## 验收

1. 两个目录均包含 `AGENTS.md`、`TEMPLATE.md` 和 `README.md`。
2. 六份 ADR 均使用日期文件名并包含全部必需章节。
3. 两份执行记录迁入 `docs/records/`，原路径不再保留重复文件。
4. ADR 与记录之间的相对链接均能解析。
5. Git 提交不包含源码文件。
