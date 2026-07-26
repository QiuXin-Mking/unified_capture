# 统一采集系统 — 分阶段测试计划

## 元数据

| 字段 | 内容 |
|------|------|
| 记录类型 | 测试计划 |
| 状态 | 已归档 |
| 开始日期 | 2026-07-23 |
| 最后更新 | 2026-07-26 |
| 负责人 | unified_capture 团队 |
| 关联计划 | 无 |

## 提取的 ADR

- [摄像头按阶段接入并设置验证门禁](../decisions/2026-07-23-staged-camera-validation.md)

> **目标:** 按 左双目 → 右双目 → 六目JHH04 → 六目JHH02 的顺序逐步加摄像头，每阶段验证采集正确性后再加下一个。

**测试环境:** RK3588 Debian 11, TSTC SDK, 所有命令在 `/root/pr-file/01-统一采集方案/unified_capture/` 下执行。

**摄像头配置表 (代码内):**

| CAMS[] 条目 | VID:PID | group_order | 分辨率 | 输出 |
|------------|---------|-------------|--------|------|
| jhh2_left  | 1bcf:2d50 | 0 | 3840×1200@30 | H.265 + Y8 |
| jhh2_right | 1bcf:2d50 | 1 | 3840×1200@30 | H.265 + Y8 |
| jhh04      | 1bcf:2d51 | 0 | 3104×480@30  | 仅 Y8 |
| jhh02      | 1bcf:2d51 | 1 | 3104×480@30  | H.265 + Y8 |

---

## 阶段 0：环境预检（每次加摄像头前必做）

### Step 0.1: 确认设备枚举

```bash
v4l2-ctl --list-devices
```

**预期:** 能看到 TSTC 设备，比如 `1bcf:2d50` 或 `1bcf:2d51`。

### Step 0.2: 确认格式支持

```bash
# JHH2 (替换 /dev/videoX 为实际设备)
v4l2-ctl -d /dev/video0 --list-formats-ext

# 六目模组
v4l2-ctl -d /dev/video4 --list-formats-ext
```

**预期:** MJPG 格式存在，分辨率与上表一致。

### Step 0.3: TSTC SDK 扫描

```bash
./unified_capture --scan
```

**预期:** 列出当前连接的设备，group_order 编号正确。

### Step 0.4: 确认依赖库可加载

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
ldd ./unified_capture | grep "not found"
```

**预期:** 无输出（所有库找到），或仅 `libUSBCam_API.so` 不在标准路径（运行时通过 LD_LIBRARY_PATH 解决）。

---

## 阶段 1：左双目单独测试

> **硬件连接:** 仅 JHH2 左目（1bcf:2d50, group_order=0）
> **目标:** 验证单路 H.265 编码 + Y8 + IMU 全链路正常。

### Step 1.1: 扫描确认

```bash
./unified_capture --scan
```

**预期输出关键行:**
```
JHH2 组 [1bcf:2d50]: 1 device(s)
  [group:0] ... (/dev/video0)
Camera mapping (CAMS → device):
  jhh2_left   → VID/PID [1bcf:2d50] group_order=0
```

jhh2_right / jhh04 / jhh02 应显示 `WARN: ... not found → disabled`。

### Step 1.2: 5 秒短录

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage1
```

**操作:** 启动后等 5 秒，按 Ctrl-C 停止。

**预期 stdout 关键行:**
```
=== Unified Capture (1 camera(s) active) ===
[jhh2_left] setup OK  (dev=/dev/video0, 3840x1200@30fps, H265=Y, Y8=Y)
>>> ALL SENSORS GO <<<
[jhh2_left] flushing encoder (N frames, X.X MB H.265)...
[jhh2_left] ffmpeg exited (0)
[jhh2_left] teardown OK
>>> Session 1 DONE <<<
```

### Step 1.3: 验证输出文件

```bash
find stage1/ -type f -ls
```

**预期文件:**
```
stage1/session_001/jhh2_left/001.mkv       # H.265 MKV, >0 字节
stage1/session_001/jhh2_left/001.y8        # Y8 灰度, >0 字节
stage1/session_001/jhh2_left/001_imu.jsonl # IMU 数据
```

**不应出现:** jhh2_right / jhh04 / jhh02 目录。

### Step 1.4: MKV 可播放性检查

```bash
ffprobe stage1/session_001/jhh2_left/001.mkv 2>&1
```

**预期:**
- Stream #0:0: Video: hevc, 3840x1200, 30 fps
- duration > 0

### Step 1.5: Y8 文件大小校验

```bash
# 3840×1200 = 4,608,000 字节/帧, 30fps × 5s ≈ 150 帧
ls -la stage1/session_001/jhh2_left/001.y8
```

**预期:** 文件大小 ≈ `帧数 × 4608000` 字节。比如 150 帧 ≈ 691MB。

### Step 1.6: IMU 数据检查

```bash
wc -l stage1/session_001/jhh2_left/001_imu.jsonl
head -3 stage1/session_001/jhh2_left/001_imu.jsonl
```

**预期:** 至少几十行（601Hz 原始采样 → 200Hz 降采样后输出）。每行 JSON 包含 `ts_us`, `ax/ay/az`, `gx/gy/gz`。

### Step 1.7: 检查异常日志

```bash
dmesg | tail -30
```

**预期:** 无 USB 断连、MPP 超时、内存不足等错误。

---

## 阶段 2：加右双目（JHH2 双路并行）

> **硬件连接:** JHH2 左目 + JHH2 右目（两个 1bcf:2d50）
> **目标:** 验证双路并行采集、设备匹配正确、编码器负载正常。

### Step 2.1: 扫描确认双设备

```bash
./unified_capture --scan
```

**预期输出关键行:**
```
JHH2 组 [1bcf:2d50]: 2 device(s)
  [group:0] ... (/dev/video0)
  [group:1] ... (/dev/video2)
Camera mapping:
  jhh2_left   → group_order=0
  jhh2_right  → group_order=1
```

### Step 2.2: 验证 group_order 匹配不交叉

两个 JHH2 外观一样，USB 枚举顺序可能随机。**核心验证:** 遮挡左摄像头，确认对应文件里画面变黑的是 jhh2_left 不是 jhh2_right。

**方法 A — 运行时查看控制台日志:**
启动程序后，观察 `[jhh2_left] setup OK (dev=/dev/videoX)` 和 `[jhh2_right] setup OK (dev=/dev/videoY)`，交叉验证 X 和 Y 分别对应哪个物理口。

**方法 B — 遮挡测试:**
```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage2
```
录 5 秒，期间用手遮住左摄像头 2 秒。停止后用 ffplay 逐帧检查：
```bash
ffplay stage2/session_001/jhh2_left/001.mkv
ffplay stage2/session_001/jhh2_right/001.mkv
```
确认遮挡的是 jhh2_left。

### Step 2.3: 5 秒双路短录

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage2
```

**预期 stdout:**
```
=== Unified Capture (2 camera(s) active) ===
[jhh2_left] setup OK  (... H265=Y, Y8=Y)
[jhh2_right] setup OK (... H265=Y, Y8=Y)
>>> ALL SENSORS GO <<<
[jhh2_left] flushing encoder (N frames, X MB H.265)...
[jhh2_right] flushing encoder (M frames, Y MB H.265)...
```

### Step 2.4: 输出文件验证

```bash
find stage2/ -type f -ls
```

**预期:** 两个目录各有 001.mkv + 001.y8 + 001_imu.jsonl，共 6 个文件。

### Step 2.5: 编码器负载检查

```bash
# 录制期间在另一个 SSH 窗口跑
cat /sys/class/mpp/mpp_service/device
# 或
cat /sys/kernel/debug/rkrga/load 2>/dev/null
```

**预期:** 没有 MPP 超时报错（dmesg 中无 `mpp_timeout` 关键字）。

### Step 2.6: 帧率一致性

```bash
# 用 ffprobe 对比两路的帧数和时长
for f in stage2/session_001/jhh2_left/001.mkv stage2/session_001/jhh2_right/001.mkv; do
    echo "=== $f ==="
    ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames,duration -of csv=p=0 "$f"
done
```

**预期:** 两路帧数接近（误差 < 5 帧），时长一致。

---

## 阶段 3：加六目 JHH04（三路混合）

> **硬件连接:** JHH2 左目 + JHH2 右目 + 六目 JHH04 (1bcf:2d51, group_order=0)
> **目标:** 验证 JHH2 与六目模组混插正常，JHH04 仅 Y8 输出。

### Step 3.1: 扫描确认

```bash
./unified_capture --scan
```

**预期:**
```
JHH2 组 [1bcf:2d50]: 2 device(s)
六目组 [1bcf:2d51]: 1 device(s)
Camera mapping:
  jhh2_left   → group_order=0
  jhh2_right  → group_order=1
  jhh04       → group_order=0
  jhh02       → WARN ... not found → disabled
```

### Step 3.2: 三路短录

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage3
```

### Step 3.3: 验证 JHH04 无 H.265（仅 Y8）

```bash
ls stage3/session_001/jhh04/
```

**预期:** 只有 `001.y8` 和 `001_imu.jsonl`，**不应有** `001.mkv`。

### Step 3.4: 验证 JHH04 IMU 码带方向

JHH04 使用 VERTICAL_LEFT 扫描策略，与 JHH2 的 HORIZONTAL_TOP 不同。

```bash
# 对比两个摄像头的 IMU 输出行数
wc -l stage3/session_001/jhh2_left/001_imu.jsonl
wc -l stage3/session_001/jhh04/001_imu.jsonl
```

**预期:** 两个都有 IMU 数据，行数 > 0。

### Step 3.5: 三路输出完整性

```bash
find stage3/ -type f | sort
```

**预期 (7 个文件):**
```
stage3/session_001/jhh2_left/001.mkv
stage3/session_001/jhh2_left/001.y8
stage3/session_001/jhh2_left/001_imu.jsonl
stage3/session_001/jhh2_right/001.mkv
stage3/session_001/jhh2_right/001.y8
stage3/session_001/jhh2_right/001_imu.jsonl
stage3/session_001/jhh04/001.y8
stage3/session_001/jhh04/001_imu.jsonl
```

### Step 3.6: 带宽/USB 稳定性检查

```bash
dmesg | grep -iE "usb|error|reset|disconnect" | tail -20
```

**预期:** 无 USB 断连或 reset 事件。

---

## 阶段 4：加六目 JHH02（四路全满）

> **硬件连接:** 全部 4 路
> **目标:** 满配压力测试，验证 4 路长时间稳定运行。

### Step 4.1: 扫描确认四路

```bash
./unified_capture --scan
```

**预期:**
```
JHH2 组 [1bcf:2d50]: 2 device(s)
六目组 [1bcf:2d51]: 2 device(s)
  jhh2_left   → group_order=0
  jhh2_right  → group_order=1
  jhh04       → group_order=0
  jhh02       → group_order=1
=== Unified Capture (4 camera(s) active) ===
```

### Step 4.2: 30 秒短录（压力初探）

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage4_short
```

### Step 4.3: 30 秒输出验证

```bash
find stage4_short/ -type f | sort
```

**预期 (10 个文件):** 4 路各 2-3 个文件（JHH04 无 mkv）。

| 摄像头 | 文件数 | 文件 |
|--------|--------|------|
| jhh2_left  | 3 | 001.mkv, 001.y8, 001_imu.jsonl |
| jhh2_right | 3 | 001.mkv, 001.y8, 001_imu.jsonl |
| jhh04      | 2 | 001.y8, 001_imu.jsonl |
| jhh02      | 3 | 001.mkv, 001.y8, 001_imu.jsonl |

### Step 4.4: 帧率一致性（4 路对比）

```bash
for cam in jhh2_left jhh2_right jhh04 jhh02; do
    dir="stage4_short/session_001/$cam"
    if [ -f "$dir/001.mkv" ]; then
        ffprobe -v error -select_streams v:0 \
            -show_entries stream=nb_frames,duration,r_frame_rate \
            -of default=noprint_wrappers=1 "$dir/001.mkv"
    fi
    echo "---"
done
```

**预期:** 有 MKV 的路 r_frame_rate=30/1，帧数 ≈ 30 × 录制秒数。

### Step 4.5: USB 带宽检查

RK3588 的 USB 3.0 理论带宽 5Gbps。4 路 MJPEG 的数据量：

| 摄像头 | 分辨率 | 每帧 (估算) | 30fps 带宽 |
|--------|--------|-----------|-----------|
| jhh2_left  | 3840×1200 | ~2MB | ~60MB/s |
| jhh2_right | 3840×1200 | ~2MB | ~60MB/s |
| jhh04      | 3104×480  | ~0.7MB | ~21MB/s |
| jhh02      | 3104×480  | ~0.7MB | ~21MB/s |
| **合计** | | | **~162MB/s ≈ 1.3Gbps** |

在 5Gbps 以内，但需要确认 USB 拓扑不共享同一个 root hub。

```bash
# 查看 USB 树
lsusb -t
```

**预期:** 摄像头分布在不同的 USB 3.0 bus 上更佳。如果挤在同一个 hub 上，注意观察是否丢帧。

### Step 4.6: CPU/内存监控

录制期间在另一个 SSH 窗口：

```bash
# 每 2 秒输出 CPU 和内存
while true; do
    top -bn1 | head -5
    sleep 2
done
```

**关注点:**
- unified_capture CPU 不过 400%（4 核满载）
- 4 个 ffmpeg 子进程各占 < 20% CPU（仅做 mux，不解码）
- 内存增长稳定（没有泄漏）

### Step 4.7: 长时间压力测试（可选，稳定后再做）

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-gpio --no-as5600 --no-vive stage4_long
```

录制 5-10 分钟，检查：
- 所有文件持续增长（用 `watch ls -lh stage4_long/session_001/*/`）
- 无 ffmpeg 异常退出
- dmesg 无 USB/MPP 错误
- 录制停止后所有 MKV 可正常播放

---

## 阶段 5：GPIO + 外设（最后）

> **前提:** 四路采集全部验证通过后
> **硬件:** 接上按钮 (gpio72) 和 AS5600 编码器

### Step 5.1: GPIO 模式录制

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture --no-as5600 --no-vive stage5_gpio
```

**操作:**
1. 看到 `GPIO ready (gpio72). Press button to start.`
2. 按下按钮 → LED 亮 → 录制开始
3. 再按按钮 → LED 灭 → 停止
4. 再按可开始下一轮 session_002
5. Ctrl-C 退出

### Step 5.2: 全功能测试（含 AS5600 + VIVE）

```bash
export LD_LIBRARY_PATH=/usr/local/TSTC/lib:/root/projects/libsurvive/bin:$LD_LIBRARY_PATH
./unified_capture stage5_full
```

**额外预期:**
- session 目录下出现 `encoder.jsonl`（AS5600 角度数据）
- AS5600 若未连接会显示 `[as5600] open /dev/i2c-6 failed` 并自动跳过，不影响采集

---

## 验证清单

每阶段通过后打勾：

### 阶段 1 □
- [ ] --scan 只显示 1 个 JHH2 设备
- [ ] 001.mkv 可播放，分辨率 3840×1200，30fps
- [ ] 001.y8 存在且大小合理
- [ ] 001_imu.jsonl 有 IMU 数据
- [ ] 无缺失文件，无 dmesg 异常

### 阶段 2 □
- [ ] --scan 显示 2 个 JHH2 设备
- [ ] group_order 匹配正确（遮挡测试通过）
- [ ] 两路 MKV 均可播放
- [ ] 帧数接近，时长一致
- [ ] 无 MPP 超时

### 阶段 3 □
- [ ] --scan 显示 2 JHH2 + 1 六目
- [ ] JHH04 无 MKV（仅 Y8），符合预期
- [ ] JHH04 IMU 有数据（VERTICAL_LEFT 策略）
- [ ] 三路共 7 个文件（2 mkv + 3 y8 + 3 imu jsonl）
- [ ] 无 USB 错误

### 阶段 4 □
- [ ] --scan 显示 2 JHH2 + 2 六目
- [ ] 4 路共 10 个文件
- [ ] 帧率正常，30fps 不掉
- [ ] CPU/内存在合理范围
- [ ] USB 带宽未超
- [ ] 长时间录制稳定

### 阶段 5 □
- [ ] GPIO 按钮启动/停止正常
- [ ] 多次 session 序号递增
- [ ] AS5600/VIVE 缺失时自动降级不崩溃

---

## 常用调试命令速查

```bash
# 设备枚举
v4l2-ctl --list-devices
./unified_capture --scan

# USB 拓扑
lsusb -t
lsusb -v -d 1bcf:2d50 2>&1 | grep -E "bcdUSB|iSerial|MaxPower"

# 实时监控文件大小
watch -n1 'find stage*/ -name "*.mkv" -exec ls -lh {} \;'

# MKV 信息
ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,nb_frames,duration \
    -of default 001.mkv

# Y8 帧数计算
echo "$(stat -c%s 001.y8) / (3840 * 1200)" | bc

# 查看 IMU 采样率
head -1 001_imu.jsonl | jq .ts_us
tail -1 001_imu.jsonl | jq .ts_us
# → (tail_ts - head_ts) / 行数 ≈ 5000us → 200Hz

# 编码器负载
dmesg | grep -i mpp
cat /proc/interrupts | grep rkv
```
