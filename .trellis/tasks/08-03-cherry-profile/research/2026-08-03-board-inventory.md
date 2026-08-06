# RK3588 Cherry 板端枚举记录

## 环境

- 主机：`root@192.168.100.200`
- 内核：Linux 5.10.160, aarch64
- USB：`Bus 002 Device 004: ID 5268:1218 YCTC YCTC_SC233HGS`
- 视频：`/dev/video0` capture，`/dev/video1` metadata
- 串口：`/dev/ttyACM0`
- 构建依赖：`g++`、`ffmpeg`、Rockchip MPP 1.3.8 和 MPP header 均存在

## USB 父路径

```text
/dev/video0
  /sys/devices/platform/fc880000.usb/usb2/2-1/2-1.1/2-1.1:1.0/video4linux/video0

/dev/ttyACM0
  /sys/devices/platform/fc880000.usb/usb2/2-1/2-1.1/2-1.1:1.2/tty/ttyACM0
```

两者的 USB device 父节点均为 `2-1.1`。单独的 `busnum=2` 无法区分同一 root hub 下的多个设备，实现应使用 USB device sysfs 父路径，同时保留 bus number 用于日志。

## UVC 实际格式

`v4l2-ctl -d /dev/video0 --list-formats-ext` 报告：

| FourCC | 3200x1200@30 | 3840x1080@30 |
| --- | --- | --- |
| `H264` | 是 | 是 |
| `HEVC` | 是 | 是 |
| `MJPG` | 是 | 是 |

当前固件没有枚举 `NV12`、`420v` 或 `2vuy`。这与原需求中“UVC NV12 原始帧 → MPP H.265”矛盾，规划时评估了：

1. 按当前硬件直接采集 UVC H.264，仅用 ffmpeg remux 到 MKV。
2. 采集 UVC MJPEG，解码后经 MPP 重编码 H.265。
3. 暂停视频实现，等待枚举 NV12 的固件后严格按原需求实现。

用户最终选择选项 1 且指定 H.264：与当前硬件一致，无 JPEG 解码和二次编码，CPU/延迟/画质代价最低，最终产物为 H.264 MKV。

## H.264 实采校验

在 `/dev/video0` 未被占用时，用 V4L2 mmap 采集 10 帧 H.264 到临时文件，采集成功：

```text
bytes=210149
codec_name=h264
width=3200
height=1200
r_frame_rate=30/1
```

`ffprobe` 能直接识别该裸流，证明当前固件的 H.264 输出可用于 `-f h264 -c copy` MKV remux。
