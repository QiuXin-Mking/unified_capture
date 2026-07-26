# Bug 文档治理设计

## 目标

在 `docs/bugs/` 集中管理 Bug 记录，并通过目录级 `AGENTS.md` 让 Codex 在该目录创建或修改 Markdown 文件时自动遵循统一规范。

## 目录设计

- `docs/bugs/AGENTS.md`：面向 Codex 的强制规则、命名要求和完成前检查项。
- `docs/bugs/TEMPLATE.md`：新 Bug 文档的标准骨架。
- `docs/bugs/README.md`：面向维护者的目录说明、状态定义和使用方法。
- `docs/bugs/YYYY-MM-DD-<slug>.md`：实际 Bug 记录。

## 文档结构

参考 `docs/2026-07-23-stage-test-plan.md` 的表达方式：标题明确、顶部给出目标与环境、用分隔线划分正文、操作命令放入代码块、每个验证步骤写清“操作”和“预期”。Bug 文档固定包含元数据、现象、影响、环境、复现步骤、预期与实际、证据、根因分析、解决或规避方案、验证结果、相关文件、经验教训和变更记录。

尚未得到证据的根因必须标记为“待确认”或“推测”，不得写成事实。未修复问题也保留验证章节，并明确标注“未验证”。

## 迁移范围

迁移两份明确的 Bug 报告：

- `docs/BUG_PTHREAD_SOCKET.md`
- `docs/tstc-sdk-bug-report.md`

`docs/01-check-data.md` 是数据查看笔记，`docs/tstc-sdk-internal-state.md` 是内部状态分析，均不迁入 Bug 目录。

## 验收

1. `docs/bugs/` 包含规则、模板、说明和两份规范化 Bug 记录。
2. Bug 文件名符合 `YYYY-MM-DD-<slug>.md`。
3. 两份记录保留原有事实，并补齐统一章节。
4. 结构检查命令确认所有必需标题存在。
