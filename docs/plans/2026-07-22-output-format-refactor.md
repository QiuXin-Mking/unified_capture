# unified_capture 输出格式重构计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让 unified_capture 支持按摄像头配置选择输出格式（H.265+Y8 或仅 Y8），并将编码器和 Tracker 输出从 CSV 改为 JSONL。

**Architecture:** 在 `CameraConfig` 加入 `output_h265` / `output_y8` 两个 bool 开关，VideoSensor 的三阶段生命周期（setup/collect/teardown）按开关走不同分支。EncoderSensor 和 ViveTrackerSensor 把 `fprintf` CSV 行改为 JSON 行。

**Tech Stack:** C++ (GCC 10+), Rockchip MPP, TSTC SDK, libgpiod, CMake/Makefile

**约束:** 嵌入式项目无单元测试框架，验证方式 = `make` 编译通过 + 代码审查。

---

### Task 1: CameraConfig 加输出控制字段

**Files:**
- Modify: `video_sensor.h:192-201`

**Step 1: 在 CameraConfig 末尾加两个 bool**

```cpp
struct CameraConfig {
    const char* name;
    uint16_t vid, pid;
    int  group_order;
    int  width, height;
    int  fps;
    int  bitrate;
    int  gop;
    bool has_imu;
    ImuOrientation imu_orientation;
    // 新增: 输出控制
    bool output_h265 = true;   // 是否编码 H.265 → MKV
    bool output_y8  = true;    // 是否写 Y8 原始灰度文件
};
```

**Step 2: 验证**

编译: `make -C unified_capture`
预期: 编译通过，无新增 warning

---

### Task 2: VideoSensor 成员变量 + setup() 条件初始化

**Files:**
- Modify: `video_sensor.h:396-421` (private 成员)
- Modify: `video_sensor.h:222-298` (setup())

**Step 1: 加 Y8 文件指针成员**

在 private 区域加:
```cpp
FILE* y8_fp_ = nullptr;
```

**Step 2: setup() 中 MPP init 加条件**

```cpp
// 将现有的 MPP init (line 249-253) 包一层:
if (cfg_.output_h265) {
    if (!mpp_.init(...)) { ... return; }
}
```

**Step 3: setup() 中 FIFO + ffmpeg 加条件**

```cpp
// 将现有的 FIFO + ffmpeg fork (line 255-290) 包一层:
if (cfg_.output_h265) {
    // mkfifo / fork ffmpeg / open fifo_fp_ 全部在这里
}
```

**Step 4: setup() 中打开 Y8 文件**

在 setup() 末尾，`initialized_ = true` 之前加:
```cpp
if (cfg_.output_y8) {
    snprintf(path, sizeof(path), "%s/%03d.y8",
             out_dir_.c_str(), session_num_);
    y8_fp_ = fopen(path, "w");
    if (!y8_fp_) {
        fprintf(stderr, "[%s] cannot create Y8 file %s\n", cfg_.name, path);
    }
}
```

**Step 5: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 3: VideoSensor::collect() 中 Y8 写入 + 条件编码

**Files:**
- Modify: `video_sensor.h:300-369` (collect())

**Step 1: MPP 编码加条件**

将 line 350:
```cpp
size_t h265_bytes = mpp_.put(nv12, fifo_fp_);
```
改为:
```cpp
size_t h265_bytes = 0;
if (cfg_.output_h265) {
    h265_bytes = mpp_.put(nv12, fifo_fp_);
}
total_h265 += h265_bytes;
```
同时把 line 311 `size_t total_h265 = 0;` 上方的声明保持不变。

**Step 2: Y8 fwrite**

紧接在 MPP 编码之后（line 351 后）加:
```cpp
if (cfg_.output_y8 && y8_fp_) {
    fwrite(nv12, 1, cfg_.width * cfg_.height, y8_fp_);
}
```

**Step 3: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 4: VideoSensor::teardown() 条件清理

**Files:**
- Modify: `video_sensor.h:371-396` (teardown())

**Step 1: FIFO/ffmpeg 清理加条件**

将 line 372 (fclose FIFO):
```cpp
if (fifo_fp_) { fclose(fifo_fp_); fifo_fp_ = nullptr; fifo_fd_ = -1; }
```
包一层:
```cpp
if (cfg_.output_h265) {
    if (fifo_fp_) { fclose(fifo_fp_); fifo_fp_ = nullptr; fifo_fd_ = -1; }
}
```

将 line 375-379 (waitpid ffmpeg):
```cpp
if (ffmpeg_pid_ > 0) { ... }
```
同样包在 `if (cfg_.output_h265)` 里。

将 line 382-384 (join stream_thread):
保持不动（TSTC 流线程始终需要 join）。

将 line 387 (MPP destroy):
```cpp
mpp_.destroy();
```
改为:
```cpp
if (cfg_.output_h265) { mpp_.destroy(); }
```

将 line 390 (unlink FIFO):
```cpp
unlink(fifo_path_.c_str());
```
改为:
```cpp
if (cfg_.output_h265) { unlink(fifo_path_.c_str()); }
```

**Step 2: 关闭 Y8 文件**

在 MPP destroy 之后加:
```cpp
if (y8_fp_) { fclose(y8_fp_); y8_fp_ = nullptr; }
```

**Step 3: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 5: main.cpp CAMS 数组配置更新

**Files:**
- Modify: `main.cpp:79-84`

**Step 1: 更新 CAMS 数组**

```cpp
static CamEntry CAMS[] = {
    {{"jhh2_left",  JHH2_VID, JHH2_PID, 0, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
    {{"jhh2_right", JHH2_VID, JHH2_PID, 1, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
    {{"jhh04",      SIX_VID,  SIX_PID,  0, 3104,  480, 30,  4000000, 30, true,  ImuOrientation::VERTICAL_LEFT,  false, true},  true, nullptr},
    {{"jhh02",      SIX_VID,  SIX_PID,  1, 3104,  480, 30,  4000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
};
```

新增的两个 bool 含义: `output_h265, output_y8`
- JHH04: `false, true` — 只要 Y8
- 其他: `true, true` — H.265 + Y8

**Step 2: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 6: EncoderSensor CSV → JSONL

**Files:**
- Modify: `encoder_sensor.h:48-52` (setup 中的 fopen + header)
- Modify: `encoder_sensor.h:70-71` (collect 中的 fprintf)

**Step 1: 改文件名**

```cpp
// 改前:
snprintf(path, sizeof(path), "%s/encoder.csv", session_dir_.c_str());
// 改后:
snprintf(path, sizeof(path), "%s/encoder.jsonl", session_dir_.c_str());
```

**Step 2: 删 header 行**

删除:
```cpp
fprintf(fp_, "ts_us,raw_angle,degrees,magnet_detected\n");
```
JSONL 不需要 header。

**Step 3: 改 fprintf 为 JSON**

```cpp
// 改前:
fprintf(fp_, "%llu,%u,%.3f,%u\n", ...);
// 改后:
fprintf(fp_, "{\"ts_us\":%llu,\"raw_angle\":%u,\"degrees\":%.3f,\"magnet_detected\":%u}\n",
        (unsigned long long)ts_now, raw, deg, magnet);
```

**Step 4: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 7: ViveTrackerSensor CSV → JSONL

**Files:**
- Modify: `vive_tracker.h:122-129` (setup 中的 fopen + header)
- Modify: `vive_tracker.h:215-221` (pose_callback 中的 fprintf)

**Step 1: 改文件名**

```cpp
// 改前:
snprintf(path, sizeof(path), "%s/tracker_pose.csv", session_dir_.c_str());
// 改后:
snprintf(path, sizeof(path), "%s/tracker.jsonl", session_dir_.c_str());
```

**Step 2: 删 header 行**

删除:
```cpp
fprintf(fp_, "ts_us,timecode,codename,x,y,z,qw,qx,qy,qz\n");
```

**Step 3: 改 fprintf 为 JSON**

```cpp
// 改前:
fprintf(s_instance_->fp_,
        "%llu,%llu,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", ...);
// 改后:
fprintf(s_instance_->fp_,
        "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\","
        "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
        "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
        ...);
```

**Step 4: 验证**

编译: `make -C unified_capture`
预期: 编译通过

---

### Task 8: 整体编译验证

**Step 1: Clean build**

```bash
make -C unified_capture clean && make -C unified_capture
```

预期: 无编译错误，无新增 warning。

**Step 2: 审查要点**

- `video_sensor.h`: JHH04 路径下 MPP/FIFO/ffmpeg 全部跳过，但 Y8 文件正常打开
- `encoder_sensor.h`: `encoder.jsonl` 输出每行 JSON，无 header
- `vive_tracker.h`: `tracker.jsonl` 输出每行 JSON，无 header
- `main.cpp`: CAMS 数组中 JHH04 的 `output_h265=false`

**Step 3: Commit**

```bash
git add unified_capture/video_sensor.h \
        unified_capture/encoder_sensor.h \
        unified_capture/vive_tracker.h \
        unified_capture/main.cpp
git commit -m "feat: per-camera output control (H.265+Y8 / Y8-only), JSONL for encoder+tracker"
```
