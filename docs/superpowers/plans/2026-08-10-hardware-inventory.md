# 硬件档案梳理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `docs/Hardware/` 建立可追溯的统一采集硬件总览和六类设备档案。

**Architecture:** 用一个总览页统一设备命名、状态语义和来源规则；每个设备页独立记录身份、接口、集成方式、验证证据和待确认项。项目源码、配置和验收记录优先于 `11-摄像头/` 的历史资料，未证实信息只作为待确认事项呈现。

**Tech Stack:** CommonMark、项目 Markdown 文档、C++ 源码注释和部署配置。

## Global Constraints

- 所有设备页均位于 `docs/Hardware/`，使用中文名称和两位数字排序。
- 事实必须能追溯到当前项目或 `11-摄像头/` 中的材料；不得补造型号、接线或镜头布局。
- 明确区分当前实现、板端验收、历史资料和待确认状态。
- 保留既有 `01-郭总摄像头.md` 的有价值内容，并按统一结构补充。

---

### Task 1: 建立硬件总览与状态规则

**Files:**
- Create: `docs/Hardware/README.md`
- Modify: `docs/Hardware/01-郭总摄像头.md`

**Interfaces:**
- Consumes: `README.md` 的硬件矩阵、`deploy/camera-map.conf.example`、`docs/TODO.md` 与 `11-摄像头/` 历史资料。
- Produces: 六篇设备页的相对链接、统一的验证状态定义和跨设备待确认清单。

- [ ] **Step 1: 写入总览页**

创建设备表，列出郭总双目、杨总独立双目、杨总六目、腕部相机、AS5600 和 VIVE Tracker 的型号/标识、接口和状态；链接至六篇设备页。

- [ ] **Step 2: 统一既有郭总摄像头页**

保留 YCTC SC233HGS、3200×1200@30、UVC、H.264/H.265/MJPEG 和 V4L2 控制参数；补充 `5268:1218`、CDC ACM 串口传感器流、5 V 220 mA 历史功耗资料及未确认事项。

- [ ] **Step 3: 检查链接和状态术语**

Run: `rg -n '\\]\\([^)]*\\.md\\)|已在项目中实现|已板端验收|历史资料|待确认' docs/Hardware`

Expected: 总览包含全部设备页链接，且状态名称与各页一致。

### Task 2: 记录三类摄像头设备

**Files:**
- Create: `docs/Hardware/02-杨总双目摄像头.md`
- Create: `docs/Hardware/03-杨总六目摄像头.md`
- Create: `docs/Hardware/04-腕部摄像头.md`

**Interfaces:**
- Consumes: `11-摄像头/01-双目摄像头信息.md`、`11-摄像头/02-六目摄像头信息.md`、`hardware/video/device_discovery.cpp`、`hardware/wrist/wrist_profile.cpp`、`docs/TODO.md`。
- Produces: 三份按设备类别独立检索的摄像头档案；README 总览使用其文件名作为稳定链接目标。

- [ ] **Step 1: 写入杨总双目设备页**

记录 JHH USB3.0 Camera/JHH2、VID/PID `1bcf:2d50`、历史序列号 JHH003、3840×1200@30，以及项目内 H.265、Y8 和 IMU JSONL 输出；注明历史 macOS 枚举的格式与当前采集实现需现场复核。

- [ ] **Step 2: 写入杨总六目设备页**

记录模组由 JHH02（双目侧，`1bcf:2d50`，4000×1200@30）和 JHH04（四目侧，`1bcf:2d51`，3104×480@30）配对；列出 JHH04 历史 NV12/YUYV 格式和不确定的六镜头物理排布。

- [ ] **Step 3: 写入腕部相机页**

记录左腕 SL、右腕 JHHSW、实测 `1bcf:2d52`、1440×960@30、顶部横向 IMU 码带、H.265 MKV 和 IMU JSONL；列出麦克风接口与实物型号待核查。

- [ ] **Step 4: 检查关键标识完整性**

Run: `rg -n '1bcf:2d50|1bcf:2d51|1bcf:2d52|3840×1200|4000×1200|3104×480|1440×960' docs/Hardware`

Expected: 三个摄像头页均包含相应的设备标识和目标采集参数。

### Task 3: 记录非视觉传感器与完成校验

**Files:**
- Create: `docs/Hardware/05-AS5600磁编码器.md`
- Create: `docs/Hardware/06-VIVE-Tracker.md`

**Interfaces:**
- Consumes: `hardware/as5600/as5600.h`、`hardware/as5600/encoder_sensor.h`、`hardware/tracker/`、`README.md`、`docs/2026-07-27-hardware-migration-sampling-validation.md`。
- Produces: 两份非视觉硬件档案，定义输出 JSONL 及其项目集成边界。

- [ ] **Step 1: 写入 AS5600 档案**

记录 I2C `0x36`、12 位 0–4095、每 LSB 0.087890625°、100 Hz 采样、磁铁状态检查，以及 `encoder-<timestamp>.jsonl` 字段。

- [ ] **Step 2: 写入 VIVE Tracker 档案**

记录 VIVE Tracker 3.0、USB HID、可选 libsurvive 依赖、原始 `tracker_raw.jsonl` 和每设备 100 Hz `tracker.jsonl`；说明历史验收中 `survive_close()` 退出阻塞仍待解决。

- [ ] **Step 3: 执行文档校验**

Run: `find docs/Hardware -maxdepth 1 -type f -name '*.md' | sort && git diff --check && rg -n '待确认|待办' docs/Hardware`

Expected: `README.md` 加六篇编号设备页均存在；差异无空白错误；每类已知信息缺口都可被检索到。
