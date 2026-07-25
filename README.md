# unified_capture

RK3588 四路摄像头统一采集程序。支持 2 路 JHH2 独立双目 + 1 台六目模组（JHH02 双目 + JHH04 四目），H.265 硬件编码 + Y8 原始灰度同步输出。

## 硬件

| 设备 | VID/PID | 分辨率 | 帧率 | 输出 |
|------|---------|--------|------|------|
| JHH2 左目 | 1bcf:2d50 | 3840×1200 | 30fps | H.265 MKV + Y8 |
| JHH2 右目 | 1bcf:2d50 | 3840×1200 | 30fps | H.265 MKV + Y8 |
| SixCam JHH02（双目） | 1bcf:2d50 | 3104×480 | 30fps | H.265 MKV + Y8 + IMU |
| SixCam JHH04（四目） | 1bcf:2d51 | 3104×480 | 30fps | Y8 + IMU |
| AS5600 编码器 | I2C 0x36 | — | 100Hz | CSV |
| VIVE Tracker 3.0 | USB HID | — | — | pose CSV |

## 依赖（RK3588 板端）

- **TSTC SDK** — USB3 Vision 摄像头驱动
- **Rockchip MPP** — H.265 硬件编码
- **libturbojpeg** — MJPEG → BGR 解码
- **libgpiod** — GPIO 按键控制
- **libsurvive** — VIVE Tracker（可选）
- **FFmpeg** — MKV 封装（运行时依赖）

## 编译

```bash
# RK3588 板端直接编译
make

# 交叉编译
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     TSTC_INC=/path/to/tstc/include TSTC_LIB=/path/to/tstc/lib
```

## 运行

### GPIO 按键模式（调试/手动）

```bash
./unified_capture /data/capture
```

按下 GPIO 按键开始录像，再按停止。支持 socket 命令并发控制。

### Socket 模式（systemd 部署）

```bash
./unified_capture --socket --no-vive /data/capture
```

启动后监听 `/tmp/unified_capture.sock`，等待 socket 命令控制启停。

### 命令行参数

```
--scan          扫描 TSTC 设备并退出
--no-vive       禁用 VIVE Tracker
--no-imu        禁用 IMU 采集
--no-as5600     禁用 AS5600 编码器
--socket        纯 socket 模式（无 GPIO），适合 systemd
```

## Socket 控制协议

Unix Domain Socket，路径 `/tmp/unified_capture.sock`，纯文本协议，每条命令以换行结束。

```bash
# 状态查询
echo "status" | nc -U /tmp/unified_capture.sock
# → {"ok":true,"ready":true,"running":false,"cameras":{"jhh2_left":true,...}}

# 开始采集
echo "start" | nc -U /tmp/unified_capture.sock
# → {"ok":true}

# 停止采集
echo "stop" | nc -U /tmp/unified_capture.sock
# → {"ok":true,"elapsed_ms":8032}
```

## systemd 部署

```bash
cp unified_capture /usr/local/bin/
cp unified_capture.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable unified_capture
systemctl start unified_capture
```

查看日志：`journalctl -u unified_capture -f`

## 输出数据结构

```
/data/capture/
└── session_001/
    ├── jhh2_left/
    │   ├── 001.mkv          # H.265 → MKV
    │   └── 001.y8           # 原始灰度
    ├── jhh2_right/
    │   ├── 001.mkv
    │   └── 001.y8
    ├── jhh02/
    │   ├── 001.mkv
    │   ├── 001.y8
    │   └── imu.csv
    ├── jhh04/
    │   ├── 001.y8
    │   └── imu.csv
    └── as5600.csv
```

## 架构

```
main (单线程 poll)
 ├── socket_setup()         → /tmp/unified_capture.sock
 ├── resolve_camera_devices()
 ├── poll(socket_fd + gpio_fd)
 └── run_session()
      ├── VideoSensor × 2   → JHH2 左/右 (各自线程)
      ├── SixCamSensor × 1  → JHH02 + JHH04 (双通道)
      ├── ImuSensor × 3     → IMU 码带解码
      ├── EncoderSensor × 1 → AS5600
      └── ViveTracker × 1   → VIVE 姿态
```

**关键设计决策：禁止 pthread_create 用于 socket**
- TSTC SDK + MPP 对额外线程敏感，`pthread_create` 会污染驱动状态
- Socket 通信合并到主线程 poll()，不创建额外线程
- 详见 [BUG_PTHREAD_SOCKET.md](BUG_PTHREAD_SOCKET.md)

## 启流顺序

1. JHH2 左目（独立）
2. JHH2 右目（独立）
3. SixCam JHH02（六目双目侧）
4. SixCam JHH04（六目四目侧，等 JHH02 完成）

同 VID/PID 设备通过 `g_stream_start_mutex` 串行化，避免 TSTC SDK 死锁。

## 文件说明

| 文件 | 说明 |
|------|------|
| `main.cpp` | 主入口，socket / GPIO / 流程控制 |
| `video_sensor.h` | JHH2 独立摄像头采集 + 编码 |
| `sixcam_sensor.h` | 六目模组双通道采集 |
| `imu_sensor.h` | IMU 码带解码传感器 |
| `encoder_sensor.h` | AS5600 磁编码器 |
| `vive_tracker.h` | VIVE Tracker 3.0 |
| `mpp_encoder.h` | Rockchip MPP H.265 编码封装 |
| `barrier.h` | SimpleBarrier（GCC 10 兼容） |
| `camera_config.h` | 摄像头配置结构 |
| `frame_queue.h` | 无锁帧队列 |
| `bgr2nv12.h` | BGR → NV12 色彩转换 |
| `imu_decode.h` | IMU 码带解码算法 |
| `vive_usb.h` | VIVE USB HID 协议 |
| `as5600.c/h` | AS5600 I2C 驱动 |
| `BUG_PTHREAD_SOCKET.md` | pthread socket 致 MPP 崩溃根因分析 |
| `SOCKET_CONTROL.md` | Socket 控制协议详细文档 |
| `unified_capture.service` | systemd unit 文件 |
| `test_socket.sh` | Socket 验收测试脚本 |
