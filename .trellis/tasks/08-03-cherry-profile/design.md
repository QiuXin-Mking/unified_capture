# Cherry Profile 技术设计

## 1. 范围和决策

本期交付一块 YCTC SC233HGS 双目板的完整采集链路：H.264 MKV、IMU、MAG、FRAME_META 和离线同步分析。腕部 cherry 只保留可复用的串口 Sensor 与配置边界，不在硬件未到货时构造虚假设备。

视频方案已由用户最终确认为直接使用当前固件枚举的 UVC H.264，由 ffmpeg 仅做 MKV remux。未采用的方案是 UVC HEVC、MJPEG 解码后 MPP 重编码（额外 CPU、延迟和画质损失），以及等待 NV12 固件（无法完成当前硬件交付）。

## 2. 组件与边界

### 2.1 配置

`ProductProfile` 增加 `cherry`。`ProductConfiguration` 增加 `CherryDeviceMap`，严格解析 `[cherry]` 中的 `stereo.vid=0x5268`、`stereo.pid=0x1218`、`stereo.resolution=3200x1200`、`stereo.format=H264`、`stereo.fps=30` 和 `allow_missing_devices`。腕部 product 名称为可选值；空值不用于当前双目发现。

### 2.2 设备发现

`hardware/cherry/cherry_discovery.*` 拥有 Cherry 端点 inventory 和纯配对函数。生产扫描从 `/sys/class/video4linux` 和 `/sys/class/tty` 沿父目录向上查找同时包含 `idVendor`/`idProduct` 的 USB device 节点，把该规范路径作为配对键。UVC 端点必须支持 `H264 3200x1200@30` 且为 Video Capture；metadata-only 节点被排除。每个 USB 父设备必须恰好对应一个视频节点和一个 tty 节点，重复或缺失都返回可诊断错误。

### 2.3 协议

`cherry_protocol.*` 实现当前采集必需的 Sensor Bridge v3 子集：CRC16、START/STOP 编码、通用帧校验、IMU/MAG/FRAME_META 载荷解码和有界流式 parser。parser 以 `53 59` 为 magic 重同步，拒绝版本、reserved、长度、类型合同或 CRC 错误，且不保留指向接收 buffer 的悬空指针。不实现当前不使用的 BYPASS、FW_VERSION 和 PWM 控制。

### 2.4 启动协调

`CherryStartControl` 是 serial/video 两个 Sensor 共享的小型状态机：`pending -> ready | failed`。Serial setup 打开 921600 8N1，发送 `START(seq=1, mask=0x07)` 并在有界超时内等待匹配响应，然后发布 ready。Video setup 等待 ready 后才 STREAMON。Serial 任何 setup 失败都发布 failed，Video 随即退出 setup；两个线程仍到达 `SimpleBarrier`，不会永久阻塞 session。

### 2.5 视频 Sensor

`V4l2Device::open` 增加 pixel format 参数，默认仍为 MJPEG，保证 mango/banana 调用不变。`CherryVideoSensor` 以 `V4L2_PIX_FMT_H264` 打开 3200x1200@30，复用 `run_capture_pipeline` 的“复制后立即 requeue”模式。处理线程把 H.264 字节原样写入 FIFO，ffmpeg 以 `-f h264 -r 30 -c copy` 生成 `cherry_stereo.mkv`；同时每帧向 `video_frames.jsonl` 写入 sequence 和 V4L2 timestamp。不创建 MPP、Y8、BGR 或码带 IMU 对象。

### 2.6 串口 Sensor

`CherrySerialSensor` 参数包含 tty path、sensor name、session directory、running flag 和 `CherryStartControl`。setup 创建三个 JSONL 文件并完成 START 握手；collect 使用 poll/read 向 parser 追加字节，按帧类型写一条 JSON；teardown 发送 `STOP(seq=2)`、允许短暂排空队列后关闭 fd 和文件。JSON 整数保留协议原始值，`uint64_t` 时间戳不经浮点转换。

### 2.7 应用集成

`CameraDiscoveryResult` 增加 cherry 视频/tty 配对。`active_profile_cameras`、status JSON 和 session 目录将其暴露为单个 `cherry_stereo` camera。`SessionRunner` 在 cherry 分支仅创建一个 SerialSensor 和一个 VideoSensor。`Runtime` 对 cherry 强制 H.264 产物，并无条件禁用 AS5600 和 VIVE；旧的 `--no-h265` 不能用于禁用 cherry 必需的 H.264 视频，如果传入则报明确错误。`use_imu` 代表是否启用 CDC sensor stream。

## 3. 数据流和输出

```text
/dev/ttyACM0 -> CherryStreamParser -> imu.jsonl
                                  -> mag.jsonl
                                  -> frame_meta.jsonl

/dev/video0 (H264) -> capture queue -> FIFO -> ffmpeg copy -> cherry_stereo.mkv
                                  -> video_frames.jsonl
```

五个文件都位于 `session_NNN/cherry_stereo/`。文件名不带 wall-clock 后缀，与需求规定的单设备目录一致。

## 4. 错误处理

- 配置错误和设备配对歧义在 Runtime 启动时失败，不进入录制。
- START 超时、ERROR 响应、tty 读写失败或 H.264 格式被驱动替换时，Sensor 打印带设备名的明确错误并安全收尾。
- parser 损坏帧会计数并继续重同步，不因单帧 CRC 错误终止长时采集。
- FIFO/ffmpeg 写入失败会标记 pipeline encoder failure（沿用现有统计枚举）并在 teardown 报告 ffmpeg 退出码。
- FRAME_META 零条是可观测的硬件接线状态，不影响 IMU/MAG 和 MKV 交付，但不标记同步验证通过。

## 5. 测试与验证

Host 测试使用已知协议字节和纯 inventory，不依赖硬件。视频 writer 使用真实临时文件验证字节不变与 JSONL 格式。分析脚本使用固定 fixture 验证最近邻域和 100ms 阈值。

板端验证顺序是：同步源码、构建与 host-only 测试、`--scan`、配置 `product=cherry`、30 秒录制、`ffprobe`、JSONL 计数/首尾样本、`calc_cherry_sync.py`。任何板端配置更改先备份原文件，不替换 systemd 服务或自动启动项。
