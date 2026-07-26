# 架构决策记录 (ADR)

## 002: 实时预览帧导出 — collect() 循环内 downscale JPEG，零新线程

**日期:** 2026-07-26

**状态:** 已采纳

**问题:**

device-ui 前端需要实时摄像头预览（`<img src="/api/camera/preview">`），但 unified_capture 当前只输出 H.265 MKV + Y8 原始灰度到磁盘。如何在不破坏现有架构的前提下提供 JPEG 预览帧？

**约束:**

1. **pthread_create 禁令:** 生产事故记录在 `BUG_PTHREAD_SOCKET.md`——在 TSTC SDK / MPP 初始化附近创建任何额外 pthread 会导致 MPP DMA buffer 分配失败、TSTC SDK STREAM_STATUS 永久死锁、所有文件产出 0 字节
2. **无新依赖:** 设备上 libjpeg-turbo 已链接（2.0.6），可直接使用
3. **性能:** 3840×1200@30fps 采集 + H.265 硬件编码已占大量 CPU，预览不能影响主采集

**方案分析:**

| 方案 | 描述 | 线程影响 | 复杂度 | 延迟 |
|------|------|---------|--------|------|
| A: 新线程 + 共享帧队列 | pthread 消费者从 FrameQueue 取 BGR 帧做 JPEG | ❌ 违反对 pthread 的禁令 | 高 | 低 |
| B: fork 子进程 + FIFO | 子进程从 FIFO 读 BGR 帧做 JPEG，进程隔离 | ✅ 不干扰 TSTC/MPP | 高 | 中 |
| C: **collect() 循环内同步完成** | 在现有 VideoSensor::collect() 中 BGR 解码后直接做 downscale + JPEG | ✅ 零新线程 | 低 | 最低 |
| D: v4l2 独立抓帧 | server.cjs 直接通过 v4l2 打开 /dev/video* 抓帧 | ✅ 不干扰采集进程 | 中 | 中 |

**决策:**

采纳 **方案 C**。

**实现细节:**

1. **触发机制:** 通过 socket 命令 `preview:<path>` 设置 `std::atomic<bool> g_preview_pending` 标志，不解码时跳过（零开销快速路径）
2. **下采样:** 最近邻 1/4 缩放 (3840×1200 → 960×300)，~1-2ms
3. **JPEG 压缩:** `tjCompress2(TJPF_BGR, TJSAMP_420, quality=85, TJFLAG_FASTDCT)`，~3-5ms
4. **原子写入:** 先写 `.tmp` 再 `rename()`，防止 server.cjs 读到半截文件
5. **总增加耗时:** <10ms/帧，30fps 帧间隔 33ms，不影响帧率

**回退:**

- 方案 B (fork 子进程) 作为备选，如果 collect() 内 JPEG 编码某天证明影响帧率
- 方案 D (v4l2 独立抓帧) 作为完全不侵入 unified_capture 的极限方案

**后果:**

- 预览固定用第一个彩色摄像头（当前 jhh2_left）。多路切换需后续 `preview:<camera>:<path>` 扩展
- 960×300 分辨率对 5.5" 屏幕的预览区域足够，但如需全屏预览需改缩放比
