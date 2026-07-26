# Bug Document Governance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立由 Codex 自动执行的 Bug 文档规范，并将现有 Bug 报告规范化归档。

**Architecture:** 使用 `docs/bugs/AGENTS.md` 提供目录级 Codex 指令，使用 `TEMPLATE.md` 提供固定结构，使用 `README.md` 解释人工工作流。实际记录使用日期前缀文件名，保留证据与验证状态。

**Tech Stack:** Markdown、Codex `AGENTS.md` 层级规则、POSIX shell 验证命令

## Global Constraints

- 仅修改 `docs/` 内的文档，不修改源码。
- 保留原 Bug 报告中的事实、命令、日志和结论。
- 未验证结论必须明确标记，不得伪造验证结果。
- 文件名使用 `YYYY-MM-DD-<lowercase-kebab-case>.md`。

---

### Task 1: 建立 Bug 文档规范

**Files:**
- Create: `docs/bugs/AGENTS.md`
- Create: `docs/bugs/TEMPLATE.md`
- Create: `docs/bugs/README.md`

**Interfaces:**
- Consumes: `docs/2026-07-23-stage-test-plan.md` 的标题、环境、步骤、命令和预期表达方式
- Produces: Codex 可自动遵循的规则和可复制的 Bug 模板

- [ ] **Step 1: 创建目录级 Codex 规则**

写明触发范围、文件命名、必需章节、证据约束和完成前自检命令。

- [ ] **Step 2: 创建标准模板**

模板覆盖元数据、复现、证据、分析、修复和验证的完整闭环。

- [ ] **Step 3: 创建目录说明**

说明新建流程、状态值、严重级别以及模板文件不属于实际 Bug 记录。

- [ ] **Step 4: 检查规范文件**

Run:

```bash
test -f docs/bugs/AGENTS.md &&
test -f docs/bugs/TEMPLATE.md &&
test -f docs/bugs/README.md
```

Expected: 退出码为 `0`。

### Task 2: 规范化现有 Bug 报告

**Files:**
- Move: `docs/BUG_PTHREAD_SOCKET.md` → `docs/bugs/2026-07-25-pthread-socket-tstc-mpp-crash.md`
- Move: `docs/tstc-sdk-bug-report.md` → `docs/bugs/2026-07-26-tstc-sdk-multi-session-issues.md`

**Interfaces:**
- Consumes: Task 1 的模板和目录规则
- Produces: 两份结构一致且保留原始证据的 Bug 记录

- [ ] **Step 1: 迁移 pthread/socket Bug**

保留故障现象、根因、排查过程、单线程 `poll` 方案和经验教训，补齐环境、复现、预期与实际、验证结果、状态及变更记录。

- [ ] **Step 2: 迁移 TSTC SDK Bug**

保留三个供应商问题及实验数据，以一个主报告记录多个相关症状，补齐统一元数据、影响、证据、规避方案、验证结果和变更记录。

- [ ] **Step 3: 执行结构验证**

Run:

```bash
for file in docs/bugs/20*.md; do
  for heading in "## 元数据" "## 现象" "## 影响" "## 环境" \
    "## 复现步骤" "## 预期结果" "## 实际结果" "## 证据" \
    "## 根因分析" "## 解决或规避方案" "## 验证结果" \
    "## 相关文件" "## 经验教训" "## 变更记录"; do
    grep -Fq "$heading" "$file" || exit 1
  done
done
```

Expected: 退出码为 `0`。

- [ ] **Step 4: 检查 Git 差异**

Run:

```bash
git diff --check
git status --short
```

Expected: `git diff --check` 无输出；状态仅包含预期文档变化和用户原有源码变化。
