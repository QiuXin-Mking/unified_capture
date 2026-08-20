# 六目四目 Y8 与 H.265 双输出实施计划

## 变更文件

- 修改 `app/session_runner.cpp`：在 mango 与 banana 的六目 `jhh04` 配置中开启 H.265 和 Y8。
- 修改 `hardware/video/sixcam_sensor.h`：仅以 `V4L2_PIX_FMT_YUYV` 打开四目 `jhh04`，保留 `jhh02` 的 MJPEG 输入。
- 新增 `hardware/video/yuyv_decoder.h`：将 packed YUYV 的 Y/U/V 字节拆为统一的 YUV 帧视图。
- 修改 `hardware/video/video_frame_processor.h`：按输入格式选择 MJPEG 解码或 YUYV 拆分。
- 修改 `app/capture_output_policy.h`：将 `mango_camera_output_policy("jhh04")` 返回值改为双输出，统一策略入口。
- 修改 `tests/test_capture_output_policy.cpp`：先锁定四目双输出契约。
- 视测试暴露的缺口，修改 `hardware/video/video_frame_processor.h` 或相关错误统计；只补必要行为，不重写已存在的 YUV/MPP 管线。
- 更新 `docs/mango-device-overview.md`、`docs/device-ui-interface.md` 或对应输出说明，记录 Y8 与 H.265 的路径和读取参数。

## 执行顺序

1. 在策略测试中把 `jhh04` 期望改为 `output_h265=true`、`output_y8=true`，运行该测试确认按 TDD 先失败。
2. 修改输出策略和两个 profile 的六目配置，重新运行策略测试确认通过。
3. 检查 `VideoFrameProcessor` 双输出路径和错误日志；若现有实现已满足要求，仅补针对性测试，不做无关重构。
4. 更新输出结构文档和板端验收命令：`wc -c` 检查 Y8、`ffprobe` 检查 MKV 的 HEVC/3104×480/30fps。
5. 运行 `make test`、`git diff --check`，在有硬件的 RK3588 上执行 60 秒验收并保存统计。

## 回滚点

若板端 30fps 不稳定，仅回退本分支中的 `jhh04` 输出开关即可恢复原有“仅 IMU”行为；不回退共享 YUV、队列或启流控制代码。

## 本次执行结果

- 已完成 `jhh04` 在 mango、banana 两条六目启动路径中的 H.265+Y8 开关。
- 已完成 mango 输出策略测试，验证 `jhh04` 双输出且腕部/`jhh02` 策略不变。
- `make test` 全部通过，`git diff --check` 通过。
- 本机生产全量编译受 macOS 缺少 `gpiod.h` 和 `linux/videodev2.h` 阻断，需在 RK3588/交叉编译环境验证。
- 尚未完成真实六目模组 60 秒板端验收。

## 实时推送扩展

- 新增 `hardware/video/y8_shared_memory.h`：8 槽 POSIX shared memory 环形队列和 Unix socket 通知。
- `jhh04` 的同一份无 stride Y8 帧同时写文件和共享内存；发布失败计入 `y8_publish_failures`，不阻塞采集。
- 新增 `docs/y8-algorithm-interface.md`，定义文件格式、握手文本、共享内存布局、丢帧和生命周期。
- 新增 `test_y8_shared_memory`，已在 macOS 主机和 RK3588 通过。
