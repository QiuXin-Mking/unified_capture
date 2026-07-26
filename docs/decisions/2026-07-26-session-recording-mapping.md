# ADR：将 Session 目录映射为 Recording 模型

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已采纳 |
| 决策日期 | 2026-07-26 |
| 最后更新 | 2026-07-26 |
| 决策人 | unified_capture / device-ui 团队 |
| 影响范围 | 采集文件索引与 device-ui 文件列表 |
| 取代 | 无 |
| 被取代于 | 无 |

---

## 背景

unified_capture 将一次采集保存为 `session_NNN/<camera>/` 目录树，其中包含 MKV、Y8 和 IMU JSONL。device-ui 文件列表使用扁平的 `Recording` 能力字段描述一条记录，需要在不改变磁盘格式的前提下建立稳定映射。

## 决策驱动因素

1. 保留 unified_capture 现有 Session 目录和多摄像头子目录。
2. 保持 device-ui 既有 `Recording` 类型和文件列表 API。
3. 映射必须通过文件事实计算，不能假设固定摄像头数量。
4. 当前系统没有深度、音频和手套数据，不应伪造能力。

## 候选方案

| 方案 | 优点 | 缺点 |
|------|------|------|
| 修改磁盘结构匹配旧 Recording | 前端后端映射简单 | 破坏采集端目录语义和多摄像头组织 |
| 前端直接遍历 Session 树 | 信息最完整 | 前端承担文件系统模型，接口变化大 |
| server.cjs 将 Session 树映射为 Recording | 保持两侧模型稳定 | 部分多摄像头语义被压缩为布尔字段 |

## 决策

由 device-ui 后端扫描 `session_NNN` 及其摄像头子目录，将每个 Session 映射为一条 `Recording`；能力字段根据实际扩展名计算，前端继续消费既有 Recording API。

## 理由

后端适配层可以同时理解磁盘布局和前端模型，避免为 UI 改写采集文件格式。基于文件存在性计算能力字段也能适应部分摄像头缺失的 Session。

## 实施约束

| Recording 字段 | 映射规则 |
|----------------|----------|
| `hasColor` | 任意摄像头子目录存在 `.mkv` |
| `hasStereo` | 当前与 `hasColor` 相同，表示多路彩色视频能力 |
| `hasImu` | 任意子目录存在 `*_imu.jsonl` |
| `hasDepth` | `false` |
| `hasGlove` | `false` |
| `hasAudio` | `false` |
| `needsDecode` | `false`，IMU 已为 JSONL |
| `decoded` | `true` |

- Session 大小递归汇总所有子目录文件。
- Session 名称和时间信息从目录及文件元数据获得。
- 新增能力字段应保持可选，避免破坏旧前端。

## 正面后果

- unified_capture 不需要改变输出目录。
- device-ui 文件列表接口保持稳定。
- 映射能根据实际文件反映部分 Session 的数据能力。
- 多摄像头文件仍保留在原始目录中。

## 负面后果

- `hasStereo = hasColor` 是兼容性近似，不能准确表达彩色摄像头数量。
- 布尔能力字段无法展示每个摄像头的独立状态。
- 递归扫描大型 Session 目录存在文件系统开销。

## 风险

| 风险 | 触发信号 | 缓解措施 |
|------|----------|----------|
| `hasStereo` 语义误导 | 单路 MKV 仍显示 stereo | 后续扩展 `cameras` 或通道计数字段 |
| 大量 Session 扫描变慢 | `/api/files` 延迟上升 | 增加索引或缓存，保持磁盘格式不变 |
| 新文件类型未被识别 | UI 能力字段与数据不符 | 在适配层集中更新扩展名映射 |

## 回退方案

若布尔模型无法承载多路采集，可在保持现有字段的同时新增可选 `cameras` 清单和每通道文件信息；旧字段继续由新模型派生，避免破坏兼容性。

## 验证方式

| 验证项 | 操作或指标 | 通过标准 | 状态 |
|--------|------------|----------|------|
| 文件列表 | 调用 `/api/files` | 返回 Session、大小和能力字段 | 已通过 |
| 彩色映射 | Session 包含 MKV | `hasColor=true` | 已通过 |
| IMU 映射 | Session 包含 `_imu.jsonl` | `hasImu=true` | 已通过 |
| 不支持能力 | 检查 depth/glove/audio | 均为 false | 已通过 |

## 相关记录

- [device-ui 集成记录](../records/2026-07-26-unified-capture-device-ui-integration-record.md)
- [device-ui 适配边界 ADR](2026-07-26-device-ui-adaptation-boundary.md)

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-26 | unified_capture / device-ui 团队 | 在集成实现中建立目录映射 |
| 2026-07-26 | Codex | 从集成记录提取为 ADR |
