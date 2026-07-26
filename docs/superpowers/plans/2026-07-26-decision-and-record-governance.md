# Decision and Record Governance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立日期命名的 ADR 与执行记录规范，将现有文档迁入正确目录，并从记录中提取六份可追溯 ADR。

**Architecture:** `docs/decisions/` 只解释“为什么选择”，`docs/records/` 保存“做了什么和结果如何”。两个目录分别使用 `AGENTS.md` 约束 Codex、`TEMPLATE.md` 固定结构、`README.md` 提供索引，ADR 与来源记录通过相对链接双向追溯。

**Tech Stack:** Markdown、Codex `AGENTS.md` 层级规则、POSIX shell 验证命令、Git

## Global Constraints

- 仅修改和提交 `docs/` 内文件。
- ADR 文件名使用 `YYYY-MM-DD-<lowercase-kebab-case>.md`。
- 每份 ADR 只记录一个可独立撤销或替换的决策。
- 原记录中的事实、命令、实验结果和待办事项不得丢失。
- 不把未验证的推测改写为已确认事实。
- Git 提交不得包含 `.h`、`.cpp` 或其他源码变化。

---

### Task 1: 建立 Decisions 与 Records 目录规范

**Files:**
- Create: `docs/decisions/AGENTS.md`
- Create: `docs/decisions/TEMPLATE.md`
- Create: `docs/decisions/README.md`
- Create: `docs/records/AGENTS.md`
- Create: `docs/records/TEMPLATE.md`
- Create: `docs/records/README.md`

**Interfaces:**
- Consumes: `docs/bugs/AGENTS.md` 的目录级治理方式
- Produces: ADR 与执行记录的命名、结构、证据和检查规则

- [ ] **Step 1: 创建 Decisions 规则、模板和索引**

规则要求日期命名、单一决策边界、固定章节、状态管理和来源链接。

- [ ] **Step 2: 创建 Records 规则、模板和索引**

规则要求保留执行证据，区分预期与实际，并将可独立决策提取到 ADR。

- [ ] **Step 3: 验证治理文件**

Run:

```bash
for dir in docs/decisions docs/records; do
  test -f "$dir/AGENTS.md"
  test -f "$dir/TEMPLATE.md"
  test -f "$dir/README.md"
done
```

Expected: 退出码为 `0`。

### Task 2: 迁移执行记录

**Files:**
- Move: `docs/2026-07-23-stage-test-plan.md` → `docs/records/2026-07-23-stage-test-plan.md`
- Move: `docs/2026-07-26-unified-capture-device-ui-integration-record.md` → `docs/records/2026-07-26-unified-capture-device-ui-integration-record.md`

**Interfaces:**
- Consumes: 两份现有原始记录
- Produces: 可被 ADR 引用的稳定来源路径

- [ ] **Step 1: 移动两份记录**

保留完整正文与 Git 历史可识别性。

- [ ] **Step 2: 添加 ADR 索引**

在两份记录顶部增加“提取的 ADR”，链接 Task 3 和 Task 4 创建的文件。

- [ ] **Step 3: 检查原路径与新路径**

Run:

```bash
test ! -e docs/2026-07-23-stage-test-plan.md
test ! -e docs/2026-07-26-unified-capture-device-ui-integration-record.md
test -f docs/records/2026-07-23-stage-test-plan.md
test -f docs/records/2026-07-26-unified-capture-device-ui-integration-record.md
```

Expected: 退出码为 `0`。

### Task 3: 规范化现有 ADR

**Files:**
- Move: `docs/decisions/001-multi-session-stability.md` → `docs/decisions/2026-07-25-multi-session-process-isolation.md`
- Move: `docs/decisions/002-preview-jpeg-in-collect-loop.md` → `docs/decisions/2026-07-26-preview-jpeg-in-collect-loop.md`

**Interfaces:**
- Consumes: 两份编号 ADR、Bug 记录和 integration record
- Produces: 两份日期命名且结构完整的 ADR

- [ ] **Step 1: 重写多 Session ADR**

保留实验、候选方案和进程隔离决策，把未获 SDK 内部证据的根因标记为推测。

- [ ] **Step 2: 重写预览 ADR**

合并最近邻缩放、JPEG 参数、原子写入和无新线程约束，并链接来源记录与 Bug。

- [ ] **Step 3: 确认编号文件已移除**

Run:

```bash
test ! -e docs/decisions/001-multi-session-stability.md
test ! -e docs/decisions/002-preview-jpeg-in-collect-loop.md
```

Expected: 退出码为 `0`。

### Task 4: 从执行记录提取 ADR

**Files:**
- Create: `docs/decisions/2026-07-23-staged-camera-validation.md`
- Create: `docs/decisions/2026-07-26-device-ui-adaptation-boundary.md`
- Create: `docs/decisions/2026-07-26-runtime-mode-auto-detection.md`
- Create: `docs/decisions/2026-07-26-session-recording-mapping.md`

**Interfaces:**
- Consumes: `docs/records/` 下两份来源记录
- Produces: 四份单一边界、可独立撤销的 ADR

- [ ] **Step 1: 提取分阶段摄像头验证 ADR**

记录串行增加硬件、每阶段设置门禁及失败时停止扩容的决策。

- [ ] **Step 2: 提取 UI 适配边界 ADR**

记录由 `server.cjs` 适配、前端接口保持向后兼容的决策。

- [ ] **Step 3: 提取运行模式自动检测 ADR**

记录通过 Unix Socket status 探测、3 秒缓存和旧链路回退的决策。

- [ ] **Step 4: 提取 Session 映射 ADR**

记录目录到前端 `Recording` 字段的映射及其限制。

### Task 5: 验证并提交

**Files:**
- Modify: `docs/decisions/README.md`
- Modify: `docs/records/README.md`

**Interfaces:**
- Consumes: Tasks 1–4 的全部文件
- Produces: 完整索引、有效链接和只含文档的 Git 提交

- [ ] **Step 1: 验证 ADR 章节与命名**

Run:

```bash
for file in docs/decisions/20*.md; do
  basename "$file" | grep -Eq '^20[0-9]{2}-[0-9]{2}-[0-9]{2}-[a-z0-9]+(-[a-z0-9]+)*\.md$'
  for heading in "## 元数据" "## 背景" "## 决策驱动因素" "## 候选方案" \
    "## 决策" "## 理由" "## 实施约束" "## 正面后果" "## 负面后果" \
    "## 风险" "## 回退方案" "## 验证方式" "## 相关记录" "## 变更记录"; do
    grep -Fq "$heading" "$file" || exit 1
  done
done
```

Expected: 退出码为 `0`。

- [ ] **Step 2: 验证 Markdown 相对链接**

使用脚本解析 `docs/decisions/*.md` 与 `docs/records/*.md` 中的相对 Markdown 链接，并确认目标存在。

- [ ] **Step 3: 检查并提交**

Run:

```bash
git diff --check
git diff --cached --check
test -z "$(git diff --cached --name-only -- '*.h' '*.hpp' '*.c' '*.cpp' '*.cc')"
```

Expected: 所有命令退出码为 `0`，随后以 `docs: organize decisions and execution records` 提交。
