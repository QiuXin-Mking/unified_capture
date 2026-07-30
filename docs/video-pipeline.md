# 视频采集与解码管线

## 总体数据流

```
USB 摄像头 (MJPEG)
  │
  ▼
V4L2 设备层 (v4l2_device.h)
  │ mmap + 非阻塞 DQBUF 轮询
  │
  ▼
MJPEG 裸码流 ──turbojpeg 解码──→ BGR24
  │
  ├──→ FrameQueue → ImuSensor (异步消费，码带解码)
  │
  └──→ bgr2nv12 → NV12 (YUV420SP, 行跨度 64 对齐)
         │
         ├─→ MPP H.265 硬件编码 → FIFO → ffmpeg 子进程 → MKV
         │
         └─→ Y8 原始灰度 (写 NV12 的 Y 平面，w×h 字节)
```

---

## 1. V4L2 设备层 — 帧采集 (`hardware/video/v4l2_device.h`)

纯 Linux V4L2/UVC，**不依赖任何 SDK**。每个摄像头对应一个 `V4l2Device` 实例。

### 初始化流程

```
open("/dev/videoN", O_RDWR | O_NONBLOCK)
  → VIDIOC_QUERYCAP          (验证是 capture 设备且支持 streaming)
  → VIDIOC_S_FMT             (协商 MJPEG 格式，记录 actual_width/actual_height)
  → VIDIOC_S_PARM            (设置帧率)
  → VIDIOC_REQBUFS           (请求 4 个 mmap buffer)
  → QUERYBUF × 4 → mmap × 4  (逐个 mmap 到用户空间)
```

关键点：
- **非阻塞打开** `O_NONBLOCK`，DQBUF 没有帧时立即返回 `EAGAIN` 而不是阻塞
- **mmap 零拷贝**：MJPEG 帧数据直接映射到用户空间，不做 `read()` 拷贝
- **4 个 buffer**：`kNumBuffers = 4`，比帧率足够
- `actual_width_` / `actual_height_` 记录 **V4L2 实际协商** 的分辨率，可能与配置文件不同，后续 MPP 和 NV12 分配都按这个来

### 采集循环

```cpp
while (running_) {
    size_t len = 0;
    uint8_t* mjpg = device.dequeue_frame(len);  // 非阻塞 DQBUF
    if (!mjpg) {
        device.wait_for_frame(10);  // poll(POLLIN, 10ms)
        continue;
    }
    // ... 处理 mjpg[len] ...
    device.requeue_frame();  // QBUF 还给内核驱动
}
```

`dequeue_frame()` 返回 `nullptr` 时不 sleep 而是 `poll()` 10ms——比固定 sleep 更高效，有帧立即返回。

---

## 2. MJPEG → BGR24 解码 (`turbojpeg`)

代码位置：`video_sensor.h:222-261`，`VideoSensor::collect()` 循环内。

### 逐个帧解码

```cpp
// 步骤 1：读 JPEG 头，获取实际尺寸
int w = 0, h = 0, subsamp = 0;
tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp);

// 步骤 2：按实际尺寸分配 BGR buffer
uint32_t bgr_size = w * h * 3;
uint8_t* bgr = new uint8_t[bgr_size];

// 步骤 3：turbojpeg 解码
int dec_ret = tjDecompress2(tj, mjpg, mjpg_len, bgr, w, 0, h,
                            TJPF_BGR, TJFLAG_FASTDCT);
```

### 为什么先读 header？

因为 **实际 JPEG 尺寸可能与 V4L2 S_FMT 协商的尺寸不同**。某些 UVC 摄像头的 MJPEG 编码器会对分辨率做微调。代码在**第一帧解码成功后才分配 NV12 buffer**，确保不会按错误尺寸分配。

```
第一帧 → tjDecompressHeader2 拿到实际 w×h → 分配 nv12 buffer（64 对齐 stride）
后续帧 → 直接复用已分配的 nv12 buffer（尺寸变化时重新分配）
```

---

## 3. BGR24 → NV12 色彩转换 (`hardware/video/bgr2nv12.h`)

纯软件实现，ITU-R BT.601 标准公式。

### 转换公式

```cpp
Y  = ( 66*R + 129*G +  25*B + 128) >> 8
U  = (-38*R -  74*G + 112*B + 128 + 32768) >> 8
V  = (112*R -  94*G -  18*B + 128 + 32768) >> 8
```

### NV12 内存布局

```
[Y 平面]   w × h 字节，行跨度 = hor_stride (64 对齐)
[UV 平面]  (w/2) × (h/2) × 2 字节，交错存储 UVUV...
```

总大小 = `hor_stride * h * 3 / 2`

### 为什么需要 64 字节对齐？

```cpp
nv12_stride_ = (w + 63) & ~63;  // MPP 硬性要求
```

RK3588 的 MPP 硬件编码器要求 NV12 行跨度 64 字节对齐，否则编码会出错。注意 `bgr_to_nv12()` 函数中 Y 平面按 `hor_stride` 寻址，UV 平面也按 `hor_stride` 寻址。

### 遍历逻辑

```cpp
for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
        // BGR24 的像素偏移
        int off = (r * w + c) * 3;
        int R = bgr[off + 2];  // BGR → R 在 +2
        int G = bgr[off + 1];  //       G 在 +1
        int B = bgr[off];      //       B 在 +0

        // 写入 Y（按 hor_stride 跨度）
        Y[r * hor_stride + c] = (66*R + 129*G + 25*B + 128) >> 8;

        // UV 只在偶数行列处写入（4:2:0 子采样）
        if ((r & 1) == 0 && (c & 1) == 0) {
            int uv_row = r / 2;
            int uv_col = c & ~1;
            int uv_idx = uv_row * hor_stride + uv_col;
            UV[uv_idx]     = U 分量;
            UV[uv_idx + 1] = V 分量;
        }
    }
}
```

---

## 4. MPP H.265 硬件编码 (`hardware/video/mpp_encoder.h`)

利用 RK3588 的 **硬件 H.265 编码器**（通过 Rockchip MPP 库）。

### 初始化

```cpp
mpp_create(&ctx, &mpi);
mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);

// Prep 配置——设置输入格式
prep.width       = w;
prep.height      = h;
prep.hor_stride  = (w + 63) & ~63;  // 64 对齐
prep.ver_stride  = h;
prep.format      = MPP_FMT_YUV420SP;

// RC 配置——码率控制
rc.rc_mode    = MPP_ENC_RC_MODE_CBR;   // 恒定比特率
rc.bps_target = bps;                   // 目标码率
rc.fps_in_num = fps;
rc.gop        = gop;                   // GOP 大小

// Codec 配置
codec.coding = MPP_VIDEO_CodingHEVC;   // H.265/HEVC

// DRM buffer group
mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM, ...);
```

### 编码一帧 — `put()`

```cpp
size_t put(uint8_t* nv12, FILE* fp) {
    // 1. 从 buffer group 获取空闲 DRM buffer
    MppBuffer buf;
    mpp_buffer_get(buf_group, &buf, frame_size);
    // 如果 buffer 暂时耗尽，等待 5ms 重试一次

    // 2. 将 NV12 拷贝到 DRM buffer
    void* ptr = mpp_buffer_get_ptr(buf);
    memcpy(ptr, nv12, frame_size);

    // 3. 构造 MppFrame → 送入编码器
    mpp_frame_set_buffer(frame, buf);
    mpi->encode_put_frame(ctx, frame);

    // 4. 循环取编码结果（可能返回多包）→ 写 FIFO
    size_t written = 0;
    MppPacket pkt;
    while (!mpi->encode_get_packet(ctx, &pkt) && pkt) {
        size_t len = mpp_packet_get_length(pkt);
        fwrite(mpp_packet_get_data(pkt), 1, len, fp);
        written += len;
        mpp_packet_deinit(&pkt);
    }
    return written;
}
```

### Flush — `flush()`

采集结束时发送 EOS 帧，排空编码器内部的残余帧：

```cpp
size_t flush(FILE* fp) {
    // 发送 EOS
    mpp_frame_set_eos(eos, 1);
    mpi->encode_put_frame(ctx, eos);

    // 取剩余包（最多 20 次）
    for (int i = 0; i < 20; i++) {
        MppPacket pkt;
        if (mpi->encode_get_packet(ctx, &pkt) || !pkt) break;
        fwrite(...);
    }
}
```

### 线程安全

`put()` 和 `flush()` 都用 `std::mutex` 保护——虽然当前每个 VideoSensor 只在单线程中调用，但 mutex 提供了防御性保护。

---

## 5. FFmpeg MKV 封装

### FIFO 机制

```cpp
// setup() 中：
mkfifo("/tmp/h265_jhh2_left_fifo", 0666);

// fork 子进程
ffmpeg_pid_ = fork();
if (ffmpeg_pid_ == 0) {
    // 子进程：读 FIFO → 封装 MKV（不转码，-c copy）
    execlp("ffmpeg", "ffmpeg",
           "-y", "-hide_banner", "-loglevel", "error",
           "-f", "hevc",              // 输入格式：裸 H.265
           "-r", "30",                // 帧率
           "-i", fifo_path,           // 输入：FIFO
           "-c", "copy",              // 不转码，纯封装
           output.mkv, NULL);
}

// 父进程打开 FIFO 写端（阻塞直到 ffmpeg 打开读端）
fifo_fd_ = open(fifo_path, O_WRONLY);
fifo_fp_ = fdopen(fifo_fd_, "w");
```

### 为什么用 FIFO 而不是 pipe？

1. **进程管理简单**：ffmpeg fork 后父进程可以用 `fopen/fwrite` 写 FIFO，不需要管 pipe 的 fd
2. **解耦**：ffmpeg 崩溃不会直接影响采集线程的写 FIFO 操作
3. **无背压**：FIFO 有内核缓冲区，encoder 输出和 ffmpeg 封装可以异步

### 结束流程

```
采集循环退出
  → mpp_.flush(fifo_fp_)    // 排空 MPP 编码器
  → fclose(fifo_fp_)        // 关闭 FIFO 写端 → ffmpeg 收到 EOF
  → waitpid(ffmpeg_pid_)    // 等 ffmpeg 正常退出
  → unlink(fifo_path)       // 删除 FIFO
```

---

## 6. Y8 原始灰度输出

Y8 不是独立提取——**直接写 NV12 的 Y 平面**：

```cpp
if (cfg_.output_y8 && y8_fp_) {
    fwrite(nv12, 1, w * h, y8_fp_);  // Y 平面 = w×h 字节
}
```

Y8 文件就是 NV12 的亮度分量，按实际 JPEG 解码尺寸写入，不是配置尺寸。

---

## 7. FrameQueue → IMU 异步消费 (`core/frame_queue.h`)

VideoSensor 和 ImuSensor 之间的桥梁。

### 生产者（Video 线程）

```cpp
// 非阻塞 push，队列满则丢弃——不允许拖慢采集
BGRFrame imu_frame(frame_idx, ts_us, w, h);
memcpy(imu_frame.data.data(), bgr, bgr_size);
bool ok = imu_queue_.try_push(std::move(imu_frame));
// ok == false → 队列满，丢弃此帧
```

### 消费者（IMU 线程）

```cpp
// 阻塞 + 非阻塞混合消费
while (running_ || !queue_.empty()) {
    BGRFrame frame;
    if (queue_.try_pop(frame)) {
        // 从 BGR 帧扫描 IMU 码带
        imu_parse_and_write(imu_buf, imu_len, frame.frame_idx, fp_);
    } else {
        usleep(5000);  // 队列空，等 5ms
    }
}
```

### 关键设计

| 设计点 | 说明 |
|--------|------|
| 队列深度 4 | 足够缓冲但不过度消耗内存 |
| `try_push` 非阻塞 | 队列满直接丢弃，不阻塞视频采集 |
| `try_pop` + sleep | IMU 线程不空转 CPU |
| `running \|\| !empty()` | 停止信号发出后仍消费残余帧 |
| `std::condition_variable` | push 时 notify，pop_wait 时可阻塞等待 |

---

## 8. 启流顺序 (`hardware/video/capture_control.h`)

不是 Nori SDK 限制，而是 **IMU 硬件依赖**。

### 依赖链

```
1. SixCam JHH02  先启流
      │
      │ control_.jhh02_init_done = true
      ▼
2. 独立 JHH2 左 / JHH2 右  并行启流
   （各自等待 jhh02_init_done，最多 10s）
      │
      │ 每个 JHH2 完成后 --control_.jhh2_remaining
      ▼
3. SixCam JHH04  等待 jhh2_remaining == 0 → 启流
```

### 为什么 JHH02 先于 JHH04？

六目模组是物理一块板，JHH02（双目）和 JHH04（四目）共享 IMU 硬件。JHH02 是 IMU 主通道，必须优先初始化。JHH04 必须等所有 JHH2（包括独立和六目中的 JHH02）都启流后才能启流。

### 协调变量

```cpp
std::atomic<int>  jhh2_remaining;    // 剩余待启流的 JHH2 设备数
std::atomic<bool> jhh02_init_done;   // JHH02 是否已启流
```

- `reset_stream_start()` 在 session 开始时根据设备数量初始化 `jhh2_remaining`
- `jhh02_init_done` 初始为 false（无六目时直接为 true）
- 等待超时：500 × 20ms = **10 秒**

---

## 9. 线程模型

```
app/main.cpp
└── Runtime (单线程 poll 主循环)
     │
     ├── SocketServer           → /tmp/unified_capture.sock
     ├── discover_cameras()     → V4L2 设备枚举
     ├── GpioControl            → GPIO 按键与指示灯
     │
     └── SessionRunner
          ├── VideoSensor (JHH2 左)     → 独立线程
          ├── VideoSensor (JHH2 右)     → 独立线程
          ├── SixCamSensor              → 独立线程
          │      ├── collect_channel(0) → 子线程 (JHH04)
          │      └── collect_channel(1) → 子线程 (JHH02)
          ├── ImuSensor × N             → 各独立线程
          ├── EncoderSensor (AS5600)    → 独立线程
          └── ViveTracker               → 独立线程
```

### SixCamSensor 的双线程

六目模组的两个通道（JHH02 + JHH04）各跑一个 `std::thread`，在 `collect_channel(0)` 和 `collect_channel(1)` 中。两个通道完全独立采集，共享同一个 `running_` 原子变量。

---

## 10. 单帧完整数据流

以 SixCam JHH02（H.265 + Y8 + IMU）为例：

```
① V4L2 DQBUF
   └→ MJPEG 裸码流 (mmap 零拷贝)

② tjDecompressHeader2
   └→ 获取 w×h

③ new bgr[w*h*3]
   └→ tjDecompress2 → BGR24

④ try_push → FrameQueue
   └→ ImuSensor 异步消费 (imu_read_frame → jsonl)

⑤ bgr_to_nv12(bgr, w, h, nv12_stride, nv12)
   └→ BGR→YUV 转换，行跨度 64 对齐

⑥ mpp_.put(nv12, fifo_fp_)
   ├→ memcpy NV12 → DRM buffer
   ├→ encode_put_frame → MPP H.265 编码
   └→ encode_get_packet → fwrite(FIFO) → ffmpeg → MKV

⑦ fwrite(nv12, 1, w×h, y8_fp_)
   └→ Y 平面 → .y8 文件

⑧ delete[] bgr
   └→ QBUF 还给内核
```

---

## 11. 设备发现与匹配 (`hardware/video/device_discovery.cpp`)

### 设备枚举

通过 sysfs `/sys/class/video4linux/` 遍历所有 V4L2 设备：

```
/sys/class/video4linux/videoN
  → 解析符号链接获取 USB 设备路径
  → 读取 idVendor / idProduct / busnum / product
  → 过滤：skip metadata-only 节点（has_video_capture_formats）
```

### Mango 设备分配逻辑

```
1. 找到 JHH04 (1bcf:2d51) → 记下其 USB bus number
2. 同一 bus 上找 JHH02 (1bcf:2d50) → 分配为 SixCam JHH02
3. 剩余 1bcf:2d50 设备按枚举顺序 → jhh2_left, jhh2_right
```

### 设备配置

| 设备 | VID/PID | 分辨率 | 帧率 | 码率 | IMU | H.265 | Y8 |
|------|---------|--------|------|------|-----|-------|----|
| JHH2 Left | 1bcf:2d50 | 3840×1200 | 30 | 16Mbps | HORIZONTAL_TOP | Y | Y |
| JHH2 Right | 1bcf:2d50 | 3840×1200 | 30 | 16Mbps | HORIZONTAL_TOP | Y | Y |
| JHH02 (SixCam) | 1bcf:2d50 | 4000×1200 | 30 | 16Mbps | HORIZONTAL_TOP | Y | Y |
| JHH04 (SixCam) | 1bcf:2d51 | 3104×480 | 30 | — | VERTICAL_LEFT | N | Y |

---

## 12. 关键代码文件索引

| 文件 | 职责 |
|------|------|
| `hardware/common/sensor.h` | Sensor 基类，三段式生命周期 |
| `hardware/video/v4l2_device.h` | V4L2 mmap 设备抽象 |
| `hardware/video/video_sensor.h` | VideoSensor 全在头文件（独立 JHH2）|
| `hardware/video/sixcam_sensor.h` | SixCamSensor（JHH02+JHH04 一体化）|
| `hardware/video/bgr2nv12.h` | BGR→NV12 色域转换 |
| `hardware/video/mpp_encoder.h` | MPP H.265 硬件编码 |
| `hardware/video/capture_control.h` | 启流顺序协调 |
| `hardware/video/device_discovery.cpp` | V4L2 设备枚举与匹配 |
| `core/frame_queue.h` | Video→IMU 帧队列 |
| `core/camera_config.h` | CameraConfig 结构体 |
| `hardware/imu/imu_sensor.h` | IMU 码带解码消费 |
