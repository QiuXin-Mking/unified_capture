# 六目四目 Y8 与 H.265 双输出设计

## 目标与范围

本任务恢复并验证 `jhh04` 的双输出能力，并为算法进程增加实时 Y8 交付。`jhh04` 使用 V4L2 YUYV 输入，按 packed YUYV 直接拆为 `DecodedYuvFrame`；Y 平面同时用于文件和共享内存，完整 YUV 可打包为 NV12 后进入 MPP。其他相机继续使用现有 MJPEG 解码链路。

## 数据流

```text
JHH04 V4L2 YUYV
  → YUYV 直接拆分为 Y/U/V 平面
      ├─ Y 平面逐行写 jhh04-<ts>.y8
      ├─ Y 平面写 8 槽共享内存环形队列→Unix socket 帧通知→算法进程
      └─ YUV→stride-aligned NV12→MPP H.265→FIFO→FFmpeg→jhh04-<ts>.mkv
```

同一个 `VideoFrameProcessor::process()` 调用完成两路输出。处理顺序保持现状：先解码与 IMU，再提交 H.265，最后写 Y8；两种输出共享同一个 `CompressedFrame` 的 frame index 和时间顺序。Y8 是无头、无 stride padding 的灰度帧串流，算法侧以 3104×480、gray8、30fps 读取；每个 Y8 像素直接来自 YUYV 的 Y 字节。

实时接口使用 `/tmp/unified_capture_jhh04_y8.sock` 握手，图像数据位于 `/unified_capture_jhh04_y8` POSIX shared memory。共享内存满时覆盖旧槽位，不阻塞 V4L2 采集；没有客户端时仍保持文件输出。

## 配置边界

- mango 分支中的 `jhh04`：`output_h265=true`、`output_y8=true`。
- banana 分支中的 `jhh04`：采用相同双输出策略，确保同一六目硬件无论通过哪个产品 profile 启动都满足需求。
- 独立腕部、`jhh02`、Cherry 的现有策略不变。
- `jhh04` 仍在 `jhh02` 和两路 wrist 启流完成后启动。

## 失败处理

沿用现有 pipeline 结果类型和统计：YUV 解码/尺寸/YUV 打包失败计入 decode failure，MPP 或 FIFO 写失败计入 encoder failure，Y8 文件打开/写失败返回输出错误并记录路径；共享内存发布失败计入 `y8_publish_failures`，但不停止文件和采集。setup 阶段打开失败直接记录 `jhh04` 与具体文件路径；teardown 仍负责 flush MPP、关闭文件并等待 FFmpeg。

## 验证

主机侧先用策略单元测试锁定 `jhh04` 双开，再运行全部 `make test`。板端使用真实六目模组采集至少 60 秒，检查文件存在性、Y8 字节数、MKV 解码信息和 pipeline 统计；保留已有未跟踪记录文件，不覆盖用户现场记录。
