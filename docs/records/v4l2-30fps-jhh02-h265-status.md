# V4L2 四路 30fps + JHH02 H.265 — 已解决

> 初始记录：2026-07-30  
> 解决与板端验收：2026-07-30  
> 验收平台：LubanCat RK3588，kernel 5.10.160  
> 目标：两路腕部 H.265、JHH02 4000×1200 H.265、JHH04 不输出视频，
> 四路 IMU 全部保留，四路相机持续以 30fps 档位采集。

## 结论

问题已解决。最终 60 秒板端验收中：

| 通道 | 获取/处理帧 | 实测处理率 | 队列溢出 | 视频输出 |
|------|-------------|------------|----------|----------|
| wrist_left | 1731 / 1731 | 29.56fps | 0 | 1440×960 HEVC |
| wrist_right | 1731 / 1731 | 29.56fps | 0 | 1440×960 HEVC |
| jhh04 | 1723 / 1723 | 29.43fps | 0 | 关闭 |
| jhh02 | 1713 / 1713 | 29.21fps | 0 | 4000×1200 HEVC |

统计窗口包含整秒启停对齐和队列排空时间，因此略低于设备配置的
30fps。所有通道均为 `acquired == processed`，没有软件流水线丢帧。
JHH02 的 V4L2 sequence 有 5 个间断（约 0.29%），但软件队列溢出、
解码失败和编码失败均为 0。

最终产物位于板端：

```text
/media/usb0/capture/validation_final_20260730/session_001
```

## 根因

原记录中的“JHH02 H.265 在约 7.5 秒、227 帧后退出”不是编码器定时
退出。受测进程仍在运行，227 帧按 30fps 封装后只显示约 7.5 秒，
实际原因是同步处理链只能处理约 9fps：

```text
V4L2 dequeue
  -> MJPEG 解码为完整 BGR
  -> BGR 转 NV12
  -> MPP 编码并等待输出
  -> FIFO 写入
  -> V4L2 requeue
```

此外还有三个独立问题叠加：

1. 禁用视频输出的通道仍执行完整解码和颜色转换。
2. 旧 `MppEncRcCfg` 先清零、再以 `change=ALL` 整体提交，把零值 QP
   字段也写入编码器，实际接近无损输出，CBR 失效，编码和写入耗时暴涨。
3. 新的亮度 IMU 解码最初在第一条部分码带后提前返回，没有像旧算法一样
   累计 12 组；JHH04 的码带方向配置也与真实帧不符。

## 实施修复

### 1. 采集与处理解耦

- V4L2 线程只复制压缩 MJPEG、保留 sequence/timestamp，并立即 requeue。
- 每路使用有界压缩帧队列和独立处理线程。
- 明确记录获取帧、处理帧、sequence gap、队列溢出和各阶段耗时。

### 2. 直接解码到 YUV

- 使用 TurboJPEG `tjDecompressToYUVPlanes()`，不再生成完整 BGR。
- 从 Y 平面直接提取 IMU。
- 仅需 H.265 的通道把 Y/U/V 平面打包为带 stride 的 NV12。
- JHH04 不输出视频，因此不再做 NV12 转换或视频写盘。

### 3. 修正 MPP CBR

- 改用 `MppEncCfg`、`MPP_ENC_GET_CFG` 和 `MPP_ENC_SET_CFG`。
- 只设置实际需要的 prep、fps、GOP、码率和 QP 项。
- 兼容板端 MPP 1.5.0 的 `rc:fps_*_denorm` 键名。

修复后 60 秒文件码率：

| 通道 | 包数 | 时长 | 平均码率 |
|------|------|------|----------|
| wrist_left | 1731 | 57.7s | 7.99Mbps |
| wrist_right | 1731 | 57.7s | 7.98Mbps |
| jhh02 | 1713 | 57.1s | 16.02Mbps |

三路均由 `ffprobe` 确认为 HEVC、30/1 帧率，码率与配置目标一致。

### 4. 解码与硬件编码流水并行

JHH02 单帧阶段耗时约为：

- MJPEG -> YUV：20.69ms
- YUV -> NV12：6.30ms
- NV12 队列提交：1.34ms
- MPP 编码：15.02ms

MPP 编码改为有界异步工作线程后，约 28ms 的预处理可以与约 15ms 的
硬件编码重叠。停止时关闭队列并完整排空，然后才 flush MPP 和关闭 FIFO。

### 5. 修复四路 IMU

- 亮度码带恢复跨扫描线累计，达到每帧 192 字节。
- 用同一张真实 MJPEG 对比旧 BGR 和新亮度算法：
  - wrist：192 / 192 字节
  - JHH02：192 / 192 字节
- 真实 JHH04 帧证明其数据沿图像行排列，配置改为
  `HORIZONTAL_TOP`。

最终 60 秒 JSONL：

| 通道 | 视频帧均成功提取 | JSONL 行数 |
|------|------------------|------------|
| wrist_left | 1731 / 1731 | 19041 |
| wrist_right | 1731 / 1731 | 19041 |
| jhh04 | 1723 / 1723 | 18953 |
| jhh02 | 1713 / 1713 | 18843 |

## 最终输出策略

- wrist_left：H.265 开，Y8 关
- wrist_right：H.265 开，Y8 关
- jhh02：H.265 开，Y8 关
- jhh04：H.265 关，Y8 关
- 四路 IMU：开

## 验证

本地和 RK3588 板端均通过完整 `make test`。板端生产构建通过。

60 秒验收日志：

```text
/tmp/four_camera_final_60s.log
```

关键结果：

```text
queue_overflows=0
decode_failures=0
encoder_failures=0
imu_overflows=0
```

原先建议的降分辨率、关闭 JHH02 H.265 或引入 RGA 均不需要。
