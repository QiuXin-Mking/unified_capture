# Cherry Profile RK3588 验收记录

日期：2026-08-03（主机）；板端时钟显示 2026-04-13，约落后 112 天，导致 make 报文件时间在未来的警告，但没有测试或链接失败。

目标板：`root@192.168.100.200`

部署目录：`/root/unified_capture`

采集目录：`/media/usb0/capture/cherry_task8_20260413_192400/session_001/cherry_stereo`

## 主机验证

以下命令均退出 0：

```bash
make clean
make test
bash tests/test_sync_to_rk3588.sh
python3 -m unittest tests/test_calc_cherry_sync.py -v
git diff --check
```

`make test` 包含全部 Cherry C++ 测试、同步工具的 3 个 Python 测试、部署脚本行为测试和源码布局测试。同步脚本的首次真实 worktree 运行暴露 `.git` 文件未排除问题，rsync 返回 23；加入回归断言并同时排除 `/.git` 与 `/.git/` 后，行为测试和真实同步均退出 0。

## 板端构建与设备确认

```bash
./deploy/sync_to_rk3588.sh
ssh root@192.168.100.200 \
  'cd /root/unified_capture && make clean && make test && make'
```

两条命令退出 0，aarch64 生产程序链接成功。板端远端仓库的 `.git` 所有者与 root 不一致，因此布局测试中的只读 `git ls-files` 打印 `dubious ownership` 警告；该检查未改变仓库或测试结果。

运行 `/root/unified_capture/unified_capture --help` 得到：

```text
Usage: ./unified_capture [OPTIONS] [output_prefix]
  --config PATH  Product configuration file
```

源码的参数解析确认本次使用的 `--no-gpio` 与 `--single` 有效。`--scan` 退出 0，报告一个 `5268:1218` 的 `YCTC_SC233HGS` V4L2 节点 `/dev/video0`。该命令当前只打印通用 V4L2 清单；实际 profile 发现日志和 sysfs/udev 进一步确认：

- `/dev/video0`：接口 `2-1.1:1.0`，VID/PID `5268:1218`；
- `/dev/ttyACM0`：接口 `2-1.1:1.2`，VID/PID `5268:1218`；
- 共同规范 USB device 父路径：`/sys/devices/platform/fc880000.usb/usb2/2-1/2-1.1`；
- `v4l2-ctl` 枚举 H.264 3200×1200，离散间隔 0.033 s（30 fps）。

## 配置保护

安装 Cherry 配置前创建了以下板端备份：

- `/etc/unified_capture/product.conf.bak.20260413_192111`
- `/etc/unified_capture/camera-map.conf.bak.20260413_192111`

活动配置为 `product=cherry`，`[cherry]` 使用 `0x5268:0x1218`、`3200x1200`、`H264`、30 fps。验收前后 `systemctl is-active unified_capture` 均为 `inactive`；没有启动、停止或重启该服务。

## 30 秒无 GPIO 会话

```bash
timeout --signal=INT --kill-after=10s 30s \
  ./unified_capture --no-gpio --single \
  /media/usb0/capture/cherry_task8_20260413_192400
```

`timeout` 按约定返回 124。程序收到 SIGINT 后打印 `Session 1 STOP` 和 `Session 1 DONE`，串口 teardown 发送 STOP；采集统计如下：

```text
Cherry serial batches imu=3089 mag=2569 frame_meta=8 parser_errors=0
ffmpeg exited (255)
PIPELINE acquired=831 processed=831 gaps=0 overflows=0 encoder_failures=0 h264_bytes=21838137
```

FFmpeg 的 255 是 timeout 向整个前台进程组发送 SIGINT 后的退出状态；程序完成 join 和 session teardown，生成的 MKV 可完整 probe 和解码计帧。

## 产物验证

`ffprobe` 退出 0：

```text
codec_name=h264
width=3200
height=1200
r_frame_rate=30/1
avg_frame_rate=30/1
nb_read_frames=830
duration=27.666000
size=21815393
```

Python 逐行 `json.loads` 解析了目录内每一个 JSONL；文件集合、大小、行数和首末样本摘要如下：

| 文件 | 大小（字节） | 行数 | 首样本 | 末样本 |
|---|---:|---:|---|---|
| `video_frames.jsonl` | 44,764 | 831 | sequence 0, timestamp 14629532599 | sequence 830, timestamp 14657203029 |
| `imu.jsonl` | 2,245,787 | 3,089 | window 14573458504..14573462504 | window 14601213373..14601221301 |
| `mag.jsonl` | 312,130 | 2,569 | pts 14573042842, raw (531110,536578,527929) | pts 14601662842, raw (526936,537457,533035) |
| `frame_meta.jsonl` | 1,336 | 8 | frame 0, sensors 0/1, pts 14573458114 | frame 7, sensors 0/1, pts 14573691435 |

FRAME_META 在本次硬件上非空，因此没有触发允许的硬件接线阻塞例外。MKV、IMU、MAG 和视频帧元数据均满足必需条件。

`python3 deploy/calc_cherry_sync.py <session_dir>` 退出 0：

```text
IMU ↔ MAG: 配对 14010 个, ≤10us: 27/14010 (0.2%)  ≤100us: 283/14010 (2.0%)  Δus: p50=2501  p90=4501  min=0  max=5000
IMU ↔ FRAME_META: 配对 168 个, ≤10us: 0/168 (0.0%)  ≤100us: 0/168 (0.0%)  Δus: p50=10992  p90=66299  min=138  max=97987
MAG ↔ FRAME_META: 配对 43 个, ≤10us: 0/43 (0.0%)  ≤100us: 0/43 (0.0%)  Δus: p50=14728  p90=75272  min=1399  max=95272
```

结论：Cherry 的 H.264 MKV、IMU、MAG、FRAME_META、视频帧元数据和离线同步工具在 RK3588 实机上完成端到端验收。`--scan` 的通用输出未展示串口/USB 父路径详情，但实际 profile 发现日志与独立 sysfs/udev 证据确认配对正确。
