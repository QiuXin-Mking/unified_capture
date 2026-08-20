origin/feature/independent-dev-update# 六目四目输出Y8与H265

## Goal

让六目模组的四目通道 `jhh04` 在统一采样 session 中同时输出算法侧可直接读取的 Y8 原始帧和 H.265 视频；保留现有 MJPEG→YUV 解码、IMU 和启流顺序。

## Requirements

- 六目四目通道 `jhh04` 使用 V4L2 YUYV 输入；按 `Y0 U Y1 V` 直接拆出 Y/U/V 平面，不经过有损压缩或颜色空间转换。
- 每个解码帧的 Y 平面按可见宽高连续写入 `jhh04/jhh04-<timestamp>.y8`，作为算法侧 Y8 输入；不写 UV 平面。
- 同一批解码帧继续送入 Rockchip MPP，生成 H.265 elementary stream，并通过现有 FFmpeg 管线封装到 `jhh04/jhh04-<timestamp>.mkv`。
- Y8 和 H.265 必须来自同一采集帧序列，不能为两种输出分别读取或丢弃帧。
- 仅 `jhh04` 请求 `V4L2_PIX_FMT_YUYV`；`jhh02`、独立 JHH2、腕部相机和 Cherry 的输入格式保持不变。
- 保留 `jhh04` 的 3104×480@30fps 配置、IMU JSONL 输出和 `jhh02 → wrist → jhh04` 的启流依赖。
- 仅修改六目四目输出策略及其必要测试/文档；不改变独立腕部、六目双目或 Cherry 的输出策略。
- 输出初始化、Y8 写盘、MPP/FIFO/FFmpeg 任一失败时必须有摄像头名和输出路径/阶段的错误日志，并在既有 pipeline 统计中可见。

## Acceptance Criteria

- [ ] `jhh04` 的配置策略断言 `output_h265=true` 且 `output_y8=true`；已有其他通道策略测试继续通过。
- [ ] `VideoFrameProcessor` 在单帧处理中同时执行 Y8 写出和 H.265 提交，且两路输出均失败时返回既有错误结果。
- [ ] 主机测试通过，包含策略、YUV/Y8 可见行写出和现有全量回归测试。
- [ ] 板端采集至少 60 秒后，`jhh04` 同时存在非空 `.y8` 与可被 FFmpeg/ffprobe 识别的 H.265 MKV；分辨率为 3104×480，帧率接近 30fps。
- [ ] 板端验收中 `jhh04` 无 sequence gap、queue overflow、decode failure 和 encoder/FIFO failure；Y8 字节数等于有效帧数×3104×480。
- [ ] 算法侧按约定路径可独立读取 Y8 原始帧，并可读取 H.265 MKV；其他通道输出不回归。

## Notes

- Keep `prd.md` focused on requirements, constraints, and acceptance criteria.
- Lightweight tasks can remain PRD-only.
- For complex tasks, add `design.md` for technical design and `implement.md` for execution planning before `task.py start`.
