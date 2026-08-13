# Bug 记录

本目录集中保存 `unified_capture` 已观察到的软硬件 Bug。测试计划、设计方案和一般排查笔记继续放在 `docs/` 的对应目录，不混入这里。

## 新建流程

1. 搜索错误码、模块名和关键日志，确认没有重复记录。
2. 复制 `TEMPLATE.md`。
3. 使用 `YYYY-MM-DD-<lowercase-kebab-case>.md` 命名。
4. 填写所有章节；没有信息时明确写“未知”或“未验证”，不要删除章节。
5. 将命令写成可直接执行的形式，并同时记录预期和实际结果。
6. 修复或规避后补充验证表和变更记录。
7. 执行 `AGENTS.md` 中的完成前检查。

## 状态

| 状态 | 含义 |
|------|------|
| 待确认 | 只有初步现象，尚未稳定复现 |
| 定位中 | 已复现，正在收集证据 |
| 已定位 | 根因已有充分证据 |
| 已规避 | 存在有效临时方案，根因未必修复 |
| 已修复 | 已实施修复，等待或已经完成验证 |
| 已关闭 | 修复通过目标环境回归，或确认不再处理 |

## 严重级别

| 级别 | 判定 |
|------|------|
| Critical | 全系统不可用、数据全部丢失或只能强制恢复 |
| High | 核心功能不可用，但存在有限规避方式 |
| Medium | 部分功能异常，不阻塞主要采集链路 |
| Low | 文档、易用性或边界行为问题 |

## 当前记录

- [2026-07-25-pthread-socket-tstc-mpp-crash.md](2026-07-25-pthread-socket-tstc-mpp-crash.md)
- [2026-07-26-tstc-sdk-multi-session-issues.md](2026-07-26-tstc-sdk-multi-session-issues.md)
- [2026-07-27-as5600-hardware-migration-remote-sync-gap.md](2026-07-27-as5600-hardware-migration-remote-sync-gap.md)
- [2026-08-13-ui-camera-preview-unavailable.md](2026-08-13-ui-camera-preview-unavailable.md)
- [2026-08-13-wrist-cameras-not-detected-mango-profile.md](2026-08-13-wrist-cameras-not-detected-mango-profile.md)
