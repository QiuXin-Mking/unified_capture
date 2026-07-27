# Nori Xvision SDK v10.00.09 迁移计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `unified_capture` 项目从旧 TSTC USBCam_API 迁移到新版 Nori Xvision SDK v10.00.09，消除多 Session 死锁 bug。

**Architecture:** 新 SDK 使用设备索引（uint32_t）替代 opaque handle，回调/轮询双模替代旧式 stream thread + DEAL_WITH 事件循环，具备 SDK 级 Init/UnInit 边界。保留 MPP/FFmpeg/Y8/IMU 管道不变，仅换 SDK 层。

**Tech Stack:** C++20, Nori Xvision API v10.00.09, Rockchip MPP, libturbojpeg, FFmpeg

**前置条件：** 新 SDK 已安装在板端 `/usr/local/Nori_Xvision/`（详见 2026-07-27 验证记录）

---

## API 映射速查

| 环节 | 旧 TSTC USBCam_API | 新 Nori Xvision v10.00.09 |
|------|-------------------|--------------------------|
| 全局初始化 | （无） | `Nori_Xvision_Init(NORI_USB_DEVICE, &count)` |
| 全局清理 | （无） | `Nori_Xvision_UnInit()` |
| 设备枚举 | `DEVICE_FIND_ID(&devs, vid, pid)` | Init + 遍历 `GetDeviceInfo(i, &info)` 匹配 VID/PID |
| 创建句柄 | `CREATE_DEVICE_POINT(dev_info)` | （不需要，直接用 device index） |
| 打开设备 | `open(Device_Path, O_RDWR)` | （SDK 内部管理） |
| 初始化采集 | `DEAL_WITH_INIT(handle, fd)` | `DeviceVideoInit(id, videoInfo)` |
| 创建流线程 | `pthread_create(stream_func, DEAL_WITH)` | （不需要，SDK 内部管理） |
| 等待流就绪 | `STREAM_STATUS(handle, 1)` | （不需要，VideoStart 返回即就绪） |
| 获取帧 | `GET_FRAME_BUFF(handle, 0)` → `Frame_Buffer_Data*` | `GetFrameBuff(id, block, msec)` → `FRAME_BUFFER_DATA*` |
| 归还帧 | `SAVE_FRAME_RES(handle, fb)` | `FreeFrameBuff(id, fb)` |
| 停止事件循环 | `EVENT_LoopMode(handle, 0)` | （不需要，VideoStop 即停止） |
| 等待流停止 | `STREAM_STATUS(handle, 0)` | （不需要） |
| 反初始化 | `DEAL_WITH_UNINIT(handle)` | `DeviceVideoUnInit(id)` |
| 销毁句柄 | `DELETE_DEVICE_POINT(handle)` | （不需要） |

### 帧缓冲字段映射

| 旧字段 | 新字段 |
|--------|--------|
| `fb->pMem` | `fb->pBufAddr` |
| `fb->buffer.bytesused` | `fb->buff_Length` |

---

### Task 1: 版本前置 — 新 SDK 安装确认

**Files:** 无代码变更

**Step 1: 验证板端 SDK 文件**

```bash
ssh root@192.168.100.200 "ls -la /usr/local/Nori_Xvision/lib/libNori_Xvision_Std.so \
  /usr/local/Nori_Xvision/include/Nori_Xvision_API/Nori_Xvision_API.h \
  /usr/local/Nori_Xvision/include/Nori_Xvision_API/Nori_Public.h \
  /usr/local/Nori_Xvision/include/Nori_Xvision_API/Nori_Error_Define.h \
  /usr/local/Nori_Xvision/include/Nori_Xvision_API/Nori_Xvision_FirmwareControl.h"
```

Expected: 5 个文件全部存在。

**Step 2: 验证旧 TSTC SDK 备份**

```bash
ssh root@192.168.100.200 "ls /usr/local/TSTC/lib/libUSBCam_API.so"
```

Expected: 旧 SDK 保留，可在回退时使用。

---

### Task 2: 更新 Makefile — 编译链接配置

**Files:**
- Modify: `Makefile`

**Step 1: 替换 Makefile 中的 TSTC 路径和库名**

将 `Makefile:17-33` 的 TSTC 相关配置改为 Nori Xvision：

```makefile
# Nori Xvision SDK 路径 (按需调整)
NORI_INC ?= /usr/local/Nori_Xvision/include
NORI_LIB ?= /usr/local/Nori_Xvision/lib
MPP_INC  ?= /usr/include/rockchip

INCLUDES := -I. \
    -I$(NORI_INC)/Nori_Xvision_API \
    -I$(MPP_INC) \
    ...(MPP/SURVIVE 路径不变)

LIBS     := -L$(NORI_LIB) -L$(SURVIVE_DIR)/bin \
    -Wl,-rpath,$(NORI_LIB) \
    -Wl,-rpath,$(SURVIVE_DIR)/bin \
    $(LDFLAGS)
```

**Step 2: 替换库名**

修改 `LDFLAGS` 行：

```makefile
LDFLAGS  := -lNori_Xvision_Std -lrockchip_mpp -lturbojpeg -lgpiod -lsurvive -lpthread -lrt -ludev -lm
```

**Step 3: 验证编译环境**

```bash
ssh root@192.168.100.200 "cd ~/unified_capture && make clean && make 2>&1 | head -20"
```

Expected: 因头文件尚未修改，编译会失败。确认错误来自 `USBCam_API.h` not found，而不是库链接错误。

---

### Task 3: 添加设备 ID 字段到 CameraConfig

**Files:**
- Modify: `camera_config.h`

**Step 1: 在 CameraConfig 中添加 device_id 字段**

在 `camera_config.h:22` 的 `bool output_y8` 之后添加：

```cpp
    bool output_h265 = true;
    bool output_y8  = true;
    int  device_id = -1;   // Nori Xvision device index, -1 = not assigned
};
```

同时添加 `#include <cstring>` 以支持后续字符串比较：

```cpp
#pragma once
#include <cstdint>
#include <cstring>
```

---

### Task 4: 重写 VideoSensor.h — 核心视频采集

**Files:**
- Modify: `hardware/VideoSensor/VideoSensor.h`

这是最关键的变更。逐段重写：

**Step 1: 头文件替换**

将 `Line 31: #include "USBCam_API.h"` 替换为：

```cpp
// Nori Xvision SDK
#include "Nori_Xvision_API.h"
```

**Step 2: 移除全局互斥锁**

删除 `Line 42-47`（`g_stream_start_mutex`, `extern g_jhh2_remaining`, `extern g_jhh02_init_done` 声明），新 SDK 不需要串行化锁。保留 preview globals (Line 50-52)。

**Step 3: 修改构造函数 — 接收 device_id 替代 dev_info**

修改 `Line 54-67`：

```cpp
class VideoSensor : public Sensor {
public:
    VideoSensor(const CameraConfig& cfg,
                const std::string& session_dir,
                uint32_t device_id,      // 替代 v4l2_dev_sys_data_t&
                int session_num,
                const std::string& session_ts,
                std::atomic<bool>& running)
        : Sensor(cfg.name, running)
        , cfg_(cfg)
        , session_dir_(session_dir)
        , device_id_(device_id)
        , session_num_(session_num)
        , session_ts_(session_ts) {}
```

**Step 4: 重写 setup() — Lines 74-206**

完整替换：

```cpp
    void setup() override {
        char path[256];
        fprintf(stderr, "[%s] DBG setup: ENTER (device_id=%u)\n", cfg_.name, device_id_);

        // ── 创建输出目录 ──
        snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), cfg_.name);
        out_dir_ = path;
        mkdir_p(out_dir_.c_str(), 0755);

        // ── 1. Nori Xvision 初始化采集 ──
        VIDEO_INFO vinfo;
        vinfo.u_Format = 0;  // 使用默认格式 (MJPEG)
        vinfo.u_Width  = (uint32_t)cfg_.width;
        vinfo.u_Height = (uint32_t)cfg_.height;
        vinfo.f_Fps    = (float)cfg_.fps;

        fprintf(stderr, "[%s] DBG setup: DeviceVideoInit id=%u %dx%d@%.1f...\n",
                cfg_.name, device_id_, vinfo.u_Width, vinfo.u_Height, vinfo.f_Fps);
        uint32_t ret = Nori_Xvision_DeviceVideoInit(device_id_, vinfo);
        if (ret != NORI_OK) {
            fprintf(stderr, "[%s] DeviceVideoInit failed: 0x%x\n", cfg_.name, ret);
            return;
        }
        fprintf(stderr, "[%s] DBG setup: DeviceVideoInit OK\n", cfg_.name);

        // 设置触发模式为非触发 (连续采集)
        E_TRIGGER_MODE mode;
        Nori_Xvision_GetTriggerMode(device_id_, &mode);
        if (mode != NON_TRIIGER_MODE) {
            Nori_Xvision_SetTriggerMode(device_id_, NON_TRIIGER_MODE);
        }

        // ── 2. MPP 编码器 (同旧代码) ──
        if (cfg_.output_h265) {
            fprintf(stderr, "[%s] DBG setup: MPP init %dx%d...\n", cfg_.name, cfg_.width, cfg_.height);
            if (!mpp_.init(cfg_.width, cfg_.height, cfg_.bitrate, cfg_.fps, cfg_.gop)) {
                fprintf(stderr, "[%s] MPP init failed\n", cfg_.name);
                return;
            }
            fprintf(stderr, "[%s] DBG setup: MPP init OK\n", cfg_.name);
        }

        // ── 3. FIFO + FFmpeg MKV 封装 (同旧代码) ──
        if (cfg_.output_h265) {
            snprintf(path, sizeof(path), "/tmp/h265_%s_fifo", cfg_.name);
            fifo_path_ = path;
            unlink(fifo_path_.c_str());
            if (mkfifo(fifo_path_.c_str(), 0666) < 0) {
                perror("mkfifo");
                return;
            }

            ffmpeg_pid_ = fork();
            if (ffmpeg_pid_ < 0) { perror("fork ffmpeg"); return; }
            if (ffmpeg_pid_ == 0) {
                snprintf(path, sizeof(path), "%s/%s-%s.mkv",
                         out_dir_.c_str(), cfg_.name, session_ts_.c_str());
                char fps_s[16];
                snprintf(fps_s, sizeof(fps_s), "%d", cfg_.fps);
                execlp("ffmpeg", "ffmpeg",
                       "-y", "-hide_banner", "-loglevel", "error",
                       "-f", "hevc", "-r", fps_s,
                       "-i", fifo_path_.c_str(),
                       "-c", "copy", path, NULL);
                perror("exec ffmpeg");
                _exit(1);
            }

            fifo_fd_ = open(fifo_path_.c_str(), O_WRONLY);
            if (fifo_fd_ < 0) { perror("open fifo for write"); return; }
            fifo_fp_ = fdopen(fifo_fd_, "w");
        }

        // ── 4. Y8 原始灰度文件 (同旧代码) ──
        if (cfg_.output_y8) {
            snprintf(path, sizeof(path), "%s/%s-%s.y8",
                     out_dir_.c_str(), cfg_.name, session_ts_.c_str());
            y8_fp_ = fopen(path, "w");
        }

        // ── 5. 启动视频流 ──
        fprintf(stderr, "[%s] DBG setup: VideoStart id=%u...\n", cfg_.name, device_id_);
        ret = Nori_Xvision_VideoStart(device_id_);
        if (ret != NORI_OK) {
            fprintf(stderr, "[%s] VideoStart failed: 0x%x\n", cfg_.name, ret);
            return;
        }
        fprintf(stderr, "[%s] DBG setup: VideoStart OK\n", cfg_.name);

        printf("[%s] setup OK  (dev_id=%u, %dx%d@%dfps, H265=%c, Y8=%c)\n",
               cfg_.name, device_id_, cfg_.width, cfg_.height, cfg_.fps,
               cfg_.output_h265 ? 'Y' : 'N', cfg_.output_y8 ? 'Y' : 'N');
        fprintf(stderr, "[%s] DBG setup: EXIT (initialized=true)\n", cfg_.name);
        initialized_ = true;
    }
```

**Step 5: 重写 collect() 帧循环 — Lines 208-382**

关键变更：`GET_FRAME_BUFF` → `GetFrameBuff`，`SAVE_FRAME_RES` → `FreeFrameBuff`，字段名 `pMem` → `pBufAddr`，`bytesused` → `buff_Length`。

同时移除 `EVENT_LoopMode` 和 `STREAM_STATUS(0)` 调用（Lines 364-366），改为在 teardown 中使用 `VideoStop`。

完整 collect() 替换：

```cpp
    void collect() override {
        if (!initialized_) {
            fprintf(stderr, "[%s] not initialized, skip\n", cfg_.name);
            return;
        }

        fprintf(stderr, "[%s] DBG collect: ENTER\n", cfg_.name);

        tjhandle tj = tjInitDecompress();
        uint64_t frame_idx = 0;
        size_t total_h265 = 0;
        int empty_polls = 0;

        uint32_t nv12_size = 0;
        uint8_t* nv12 = nullptr;
        int nv12_w = 0, nv12_h = 0;

        while (running_) {
            // ★ 新 API: GetFrameBuff(id, block=false, timeout=0) — 非阻塞轮询
            FRAME_BUFFER_DATA* fb = Nori_Xvision_GetFrameBuff(device_id_, false, 0);
            if (!fb) {
                empty_polls++;
                if (empty_polls == 1) {
                    fprintf(stderr, "[%s] DBG collect: GetFrameBuff NULL (first)\n", cfg_.name);
                }
                usleep(1000);
                continue;
            }

            if (empty_polls > 0) {
                fprintf(stderr, "[%s] DBG collect: first frame after %d empty polls\n",
                        cfg_.name, empty_polls);
                empty_polls = 0;
            }

            uint64_t ts_us = elapsed_us();
            uint8_t* mjpg = (uint8_t*)fb->pBufAddr;     // ★ 旧: pMem
            size_t mjpg_len = fb->buff_Length;           // ★ 旧: buffer.bytesused

            // --- MJPEG → BGR (turbojpeg) ---
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000 || h <= 0 || h > 8000) {
                Nori_Xvision_FreeFrameBuff(device_id_, fb);  // ★ 旧: SAVE_FRAME_RES
                continue;
            }

            // ★ 按实际 JPEG 尺寸分配 nv12
            if (!nv12 || w != nv12_w || h != nv12_h) {
                delete[] nv12;
                nv12_w = w; nv12_h = h;
                nv12_size = w * h * 3 / 2;
                nv12 = new (std::nothrow) uint8_t[nv12_size];
                if (!nv12) {
                    fprintf(stderr, "[%s] FATAL: nv12 alloc failed (w=%d h=%d)\n", cfg_.name, w, h);
                    Nori_Xvision_FreeFrameBuff(device_id_, fb);
                    break;
                }
                fprintf(stderr, "[%s] actual resolution %dx%d (cfg=%dx%d)\n",
                        cfg_.name, w, h, cfg_.width, cfg_.height);
            }

            uint32_t bgr_size = w * h * 3;
            uint8_t* bgr = new uint8_t[bgr_size];
            int dec_ret = tjDecompress2(tj, mjpg, mjpg_len, bgr, w, 0, h,
                                        TJPF_BGR, TJFLAG_FASTDCT);

            if (dec_ret == 0) {
                if (cfg_.has_imu) {
                    BGRFrame imu_frame(frame_idx, ts_us, w, h);
                    memcpy(imu_frame.data.data(), bgr, bgr_size);
                    imu_queue_.try_push(std::move(imu_frame));
                }

                bgr_to_nv12(bgr, w, h, nv12);

                if (cfg_.output_h265) {
                    size_t h265_bytes = mpp_.put(nv12, fifo_fp_);
                    total_h265 += h265_bytes;
                }

                if (cfg_.output_y8 && y8_fp_) {
                    fwrite(nv12, 1, w * h, y8_fp_);
                }

                // Preview JPEG export (同旧代码, 保持不变)
                if (g_preview_pending.load()) {
                    std::lock_guard<std::mutex> lock(g_preview_mutex);
                    if (g_preview_pending.load()) {
                        int pw = w / 4, ph = h / 4;
                        if (pw < 1) { pw = 1; } if (ph < 1) { ph = 1; }
                        std::vector<uint8_t> scaled(pw * ph * 3);
                        for (int y = 0; y < ph; y++) {
                            for (int x = 0; x < pw; x++) {
                                int sx = x * 4, sy = y * 4;
                                int src_off = (sy * w + sx) * 3;
                                int dst_off = (y * pw + x) * 3;
                                scaled[dst_off]   = bgr[src_off];
                                scaled[dst_off+1] = bgr[src_off+1];
                                scaled[dst_off+2] = bgr[src_off+2];
                            }
                        }
                        unsigned long jpg_size = 0;
                        uint8_t* jpg_buf = nullptr;
                        tjhandle jpg_h = tjInitCompress();
                        if (jpg_h) {
                            int jr = tjCompress2(jpg_h, scaled.data(), pw, 0, ph,
                                                 TJPF_BGR, &jpg_buf, &jpg_size,
                                                 TJSAMP_420, 85, TJFLAG_FASTDCT);
                            if (jr == 0 && jpg_buf && jpg_size > 0) {
                                std::string tmp = g_preview_path + ".tmp";
                                FILE* fp = fopen(tmp.c_str(), "wb");
                                if (fp) {
                                    fwrite(jpg_buf, 1, jpg_size, fp);
                                    fclose(fp);
                                    rename(tmp.c_str(), g_preview_path.c_str());
                                }
                                tjFree(jpg_buf);
                            }
                            tjDestroy(jpg_h);
                        }
                        g_preview_pending = false;
                    }
                }
            } else if (frame_idx == 0) {
                fprintf(stderr, "[%s] DBG collect: tjDecompress2 failed, ret=%d\n",
                        cfg_.name, dec_ret);
            }

            delete[] bgr;
            Nori_Xvision_FreeFrameBuff(device_id_, fb);  // ★ 旧: SAVE_FRAME_RES
            frame_idx++;

            if (frame_idx % 30 == 0) {
                fprintf(stderr, "[%s] DBG collect: frame=%llu, last_mjpg_len=%zu, h265_total=%zu\n",
                        cfg_.name, (unsigned long long)frame_idx, mjpg_len, total_h265);
            }
        }

        fprintf(stderr, "[%s] DBG collect: loop exit (frame_idx=%llu, empty_polls=%d)\n",
                cfg_.name, (unsigned long long)frame_idx, empty_polls);

        // 停止流
        fprintf(stderr, "[%s] DBG collect: VideoStop...\n", cfg_.name);
        Nori_Xvision_VideoStop(device_id_);

        // Flush MPP
        if (cfg_.output_h265) {
            printf("[%s] flushing encoder (%zu frames, %.1f MB H.265)...\n",
                   cfg_.name, frame_idx, total_h265 / 1048576.0);
            mpp_.flush(fifo_fp_);
        } else {
            printf("[%s] stopped (%zu frames)\n", cfg_.name, frame_idx);
        }

        delete[] nv12;
        tjDestroy(tj);
        fprintf(stderr, "[%s] DBG collect: EXIT\n", cfg_.name);
    }
```

**Step 6: 重写 teardown() — Lines 384-430**

移除旧 stream_thread 等待和 DEAL_WITH_UNINIT/DELETE_DEVICE_POINT，替换为 DeviceVideoUnInit：

```cpp
    void teardown() override {
        fprintf(stderr, "[%s] DBG teardown: ENTER\n", cfg_.name);

        if (cfg_.output_h265) {
            if (fifo_fp_) {
                fclose(fifo_fp_); fifo_fp_ = nullptr; fifo_fd_ = -1;
            }
            if (ffmpeg_pid_ > 0) {
                int status;
                waitpid(ffmpeg_pid_, &status, 0);
                printf("[%s] ffmpeg exited (%d)\n", cfg_.name, WEXITSTATUS(status));
            }
        }

        // ★ 新 API: 反初始化设备
        fprintf(stderr, "[%s] DBG teardown: DeviceVideoUnInit...\n", cfg_.name);
        Nori_Xvision_DeviceVideoUnInit(device_id_);

        // 清理 MPP
        if (cfg_.output_h265) {
            mpp_.destroy();
        }

        // 清理 FIFO
        if (cfg_.output_h265) {
            unlink(fifo_path_.c_str());
        }

        // 关闭 Y8 文件
        if (y8_fp_) { fclose(y8_fp_); y8_fp_ = nullptr; }

        printf("[%s] teardown OK\n", cfg_.name);
        fprintf(stderr, "[%s] DBG teardown: EXIT\n", cfg_.name);
    }
```

**Step 7: 修改私有成员 — Lines 432-476**

替换 Line 438-445（旧的 TSTC 成员）：

```cpp
private:
    CameraConfig cfg_;
    std::string session_dir_;
    std::string out_dir_;
    uint32_t device_id_;          // ★ 旧: v4l2_dev_sys_data_t& dev_info_
    int session_num_;
    std::string session_ts_;

    // MPP (同旧代码)
    MppEncoder mpp_;

    // FFmpeg (同旧代码)
    pid_t ffmpeg_pid_ = 0;
    std::string fifo_path_;
    int fifo_fd_ = -1;
    FILE* fifo_fp_ = nullptr;

    // Y8 (同旧代码)
    FILE* y8_fp_ = nullptr;

    // IMU queue (同旧代码)
    FrameQueue imu_queue_{4};

    bool initialized_ = false;

    // ★ 不再需要 stream_thread_func / stream_thread_
};
```

删除整个 `stream_thread_func` 静态方法（Lines 464-476），新 SDK 不使用独立的流线程。

---

### Task 5: 重写 SixCamSensor.h — 六目模组双通道

**Files:**
- Modify: `hardware/VideoSensor/SixCamSensor.h`

**Step 1: 头文件替换**

Line 30: `#include "USBCam_API.h"` → `#include "Nori_Xvision_API.h"`

删除 extern 声明（Lines 39-41）中的 `g_stream_start_mutex`，保留 `g_jhh2_remaining` 和 `g_jhh02_init_done`（用于 IMU 启流顺序）。

**Step 2: 更新 SixCamChannel 结构体 — Lines 51-82**

将 TSTC 相关字段改为：

```cpp
struct SixCamChannel {
    const char* name;
    int width, height, fps, bitrate, gop;
    bool output_h265;
    bool output_y8;
    bool has_imu;
    ImuOrientation imu_orientation;

    // Nori Xvision
    uint32_t device_id = 0;   // ★ 旧: void* tstc_handle + int dev_fd + pthread_t stream_thread

    // MPP/FFmpeg/Y8 (同旧代码, 保持不变)
    MppEncoder mpp;
    pid_t      ffmpeg_pid = 0;
    std::string fifo_path;
    int         fifo_fd = -1;
    FILE*       fifo_fp = nullptr;
    FILE*       y8_fp = nullptr;
    FrameQueue  imu_queue{4};
    std::string out_dir;
    bool initialized = false;
};
```

**Step 3: 更新构造函数 — Lines 89-129**

接收 `uint32_t jhh04_id, uint32_t jhh02_id` 替代 `v4l2_dev_sys_data_t&`：

```cpp
    SixCamSensor(const CameraConfig& jhh04_cfg,
                 const CameraConfig& jhh02_cfg,
                 uint32_t jhh04_id,
                 uint32_t jhh02_id,
                 const std::string& session_dir,
                 int session_num,
                 const std::string& session_ts,
                 std::atomic<bool>& running)
        : Sensor("sixcam", running)
        , session_dir_(session_dir)
        , session_num_(session_num)
        , session_ts_(session_ts)
    {
        // ch_[0] = jhh04, ch_[1] = jhh02 (同旧代码的配置拷贝)
        // ...
        ch_[0].device_id = jhh04_id;
        ch_[1].device_id = jhh02_id;
    }
```

删除 `jhh04_dev_` 和 `jhh02_dev_` 成员。

**Step 4: 重写 setup() — Lines 137-283**

关键变更：
- 移除 CREATE_DEVICE_POINT + open + DEAL_WITH_INIT + pthread_create(stream_thread) + STREAM_STATUS
- 替换为 DeviceVideoInit + VideoStart
- **保留启流顺序**：jhh02 先启（设置 g_jhh02_init_done），jhh04 等 g_jhh2_remaining 归零后启
- 移除 g_stream_start_mutex（新 SDK 不需要）

```cpp
    void setup() override {
        char path[256];

        // ── 1. 创建输出目录 (同旧代码) ──
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), ch_[i].name);
            ch_[i].out_dir = path;
            mkdir_p(path, 0755);
        }

        // ── 2. DeviceVideoInit 两个通道 ──
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            VIDEO_INFO vinfo;
            vinfo.u_Format = 0;
            vinfo.u_Width  = (uint32_t)ch.width;
            vinfo.u_Height = (uint32_t)ch.height;
            vinfo.f_Fps    = (float)ch.fps;

            fprintf(stderr, "[%s] DBG: DeviceVideoInit id=%u %dx%d@%.1f...\n",
                    ch.name, ch.device_id, vinfo.u_Width, vinfo.u_Height, vinfo.f_Fps);
            uint32_t ret = Nori_Xvision_DeviceVideoInit(ch.device_id, vinfo);
            if (ret != NORI_OK) {
                fprintf(stderr, "[%s] DeviceVideoInit failed: 0x%x\n", ch.name, ret);
                return;
            }
            fprintf(stderr, "[%s] DBG: DeviceVideoInit OK\n", ch.name);

            E_TRIGGER_MODE mode;
            Nori_Xvision_GetTriggerMode(ch.device_id, &mode);
            if (mode != NON_TRIIGER_MODE) {
                Nori_Xvision_SetTriggerMode(ch.device_id, NON_TRIIGER_MODE);
            }
        }

        // ── 3. MPP (同旧代码) ──
        // ...

        // ── 4. FIFO + FFmpeg (同旧代码) ──
        // ...

        // ── 5. Y8 文件 (同旧代码) ──
        // ...

        // ── 6. JHH02 先启流 (保留 IMU 依赖顺序) ──
        {
            auto& ch = ch_[1];  // jhh02
            fprintf(stderr, "[%s] DBG: VideoStart (first, IMU master)...\n", ch.name);
            uint32_t ret = Nori_Xvision_VideoStart(ch.device_id);
            if (ret != NORI_OK) {
                fprintf(stderr, "[%s] VideoStart failed: 0x%x\n", ch.name, ret);
                return;
            }
            g_jhh02_init_done = true;
            int rem = --g_jhh2_remaining;
            fprintf(stderr, "[%s] DBG: JHH2 done, remaining=%d\n", ch.name, rem);
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }

        // ── 7. JHH04 后启流 ──
        {
            auto& ch = ch_[0];  // jhh04
            fprintf(stderr, "[%s] DBG: waiting for %d JHH2 devices...\n",
                    ch.name, (int)g_jhh2_remaining);
            while (g_jhh2_remaining > 0) {
                usleep(20000);
            }
            fprintf(stderr, "[%s] DBG: all JHH2 done, VideoStart...\n", ch.name);
            uint32_t ret = Nori_Xvision_VideoStart(ch.device_id);
            if (ret != NORI_OK) {
                fprintf(stderr, "[%s] VideoStart failed: 0x%x\n", ch.name, ret);
                return;
            }
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }
    }
```

**Step 5: 更新 collect_channel() 帧循环 — Lines 357-513**

同 VideoSensor 的变更模式：`GET_FRAME_BUFF` → `GetFrameBuff`，`SAVE_FRAME_RES` → `FreeFrameBuff`，`pMem` → `pBufAddr`，`bytesused` → `buff_Length`。

**Step 6: 重写 teardown() — Lines 294-331**

移除 `EVENT_LoopMode`、`STREAM_STATUS(0)`、`pthread_join(stream_thread)`、`DEAL_WITH_UNINIT`、`DELETE_DEVICE_POINT`。

替换为：

```cpp
    void teardown() override {
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (!ch.initialized) continue;

            fprintf(stderr, "[%s] DBG teardown: VideoStop...\n", ch.name);
            Nori_Xvision_VideoStop(ch.device_id);

            if (ch.output_h265) {
                if (ch.fifo_fp) { fclose(ch.fifo_fp); ch.fifo_fp = nullptr; }
                if (ch.ffmpeg_pid > 0) {
                    int status;
                    waitpid(ch.ffmpeg_pid, &status, 0);
                    printf("[%s] ffmpeg exited (%d)\n", ch.name, WEXITSTATUS(status));
                }
            }

            if (ch.output_h265) { unlink(ch.fifo_path.c_str()); }
            if (ch.y8_fp) { fclose(ch.y8_fp); ch.y8_fp = nullptr; }

            fprintf(stderr, "[%s] DBG teardown: DeviceVideoUnInit...\n", ch.name);
            Nori_Xvision_DeviceVideoUnInit(ch.device_id);

            printf("[%s] teardown OK\n", ch.name);
        }
    }
```

**Step 7: 删除 stream_thread_func**

删除 `static void* stream_thread_func(void* arg)` (Lines 334-343)，不再需要流线程。

**Step 8: 删除 SixCamSensor 的旧 TSTC 成员**

删除 `v4l2_dev_sys_data_t jhh04_dev_` 和 `jhh02_dev_` (Lines 351-352)。

---

### Task 6: 重写 main.cpp — 设备枚举和会话生命周期

**Files:**
- Modify: `main.cpp`

**Step 1: 头文件替换**

Line 20-22:

```cpp
extern "C" {
#include "Nori_Xvision_API.h"
}
```

**Step 2: 删除全局互斥锁声明**

删除 `g_stream_start_mutex` 声明（如果有的话），保留 `g_jhh2_remaining` 和 `g_jhh02_init_done`。

**Step 3: 重写设备枚举 — `resolve_camera_devices()` (Lines 140-193)**

完全替换设备匹配逻辑。新 API 没有 `DEVICE_FIND_ID`，改为自己枚举：

```cpp
struct VidPidGroup { uint16_t vid, pid; std::vector<uint32_t> device_ids; };

static int resolve_camera_devices() {
    uint32_t total_devices = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &total_devices);
    if (ret != NORI_OK) {
        fprintf(stderr, "ERROR: Nori_Xvision_Init failed: 0x%x\n", ret);
        return 0;
    }
    printf("Nori Xvision SDK: found %u device(s)\n", total_devices);

    if (total_devices == 0) {
        Nori_Xvision_UnInit();
        return 0;
    }

    // 按 VID/PID 分组所有设备
    std::vector<VidPidGroup> groups;
    for (uint32_t i = 0; i < total_devices; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);

        VidPidGroup* grp = nullptr;
        for (auto& g : groups) {
            if (g.vid == info.idVendor && g.pid == info.idProduct) { grp = &g; break; }
        }
        if (!grp) {
            VidPidGroup g;
            g.vid = info.idVendor;
            g.pid = info.idProduct;
            groups.push_back(g);
            grp = &groups.back();
        }
        grp->device_ids.push_back(i);  // 同一个 VID/PID 组内按 Init 顺序

        printf("  Device[%u]: %04x:%04x \"%s\" \"%s\" %s\n",
               i, info.idVendor, info.idProduct,
               info.iManufacturer, info.iProduct, info.device);
    }

    // 匹配 JHH2 独立相机 (1bcf:2d50, 取前 2 个)
    VidPidGroup* jhh2_grp = nullptr;
    for (auto& g : groups) {
        if (g.vid == JHH2_VID && g.pid == JHH2_PID) { jhh2_grp = &g; break; }
    }
    if (jhh2_grp) {
        for (int i = 0; i < N_CAMS && i < (int)jhh2_grp->device_ids.size(); i++) {
            auto& cam = CAMS[i];
            if (!cam.enabled) continue;
            cam.cfg.device_id = jhh2_grp->device_ids[i];
            cam.enabled = true;
            DEVICE_INFO info;
            Nori_Xvision_GetDeviceInfo(cam.cfg.device_id, &info);
            printf("  %-12s -> device[%u] %s  %dx%d@%d IMU=%c\n",
                   cam.cfg.name, cam.cfg.device_id, info.device,
                   cam.cfg.width, cam.cfg.height, cam.cfg.fps,
                   cam.cfg.has_imu ? 'Y' : 'N');
        }
    }

    // 匹配六目模组
    for (auto& g : groups) {
        if (g.vid == SIX_VID && g.pid == SIX_PID && !g.device_ids.empty()) {
            g_sixcam.jhh04_id = g.device_ids[0];
            g_sixcam.enabled = true;
            DEVICE_INFO info;
            Nori_Xvision_GetDeviceInfo(g_sixcam.jhh04_id, &info);
            printf("  %-12s -> device[%u] %s  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh04", g_sixcam.jhh04_id, info.device);
        }
    }

    // jhh02 (六目双目侧): 从 jhh2_grp 中取 group_order=2
    if (g_sixcam.enabled && jhh2_grp && jhh2_grp->device_ids.size() >= 3) {
        g_sixcam.jhh02_id = jhh2_grp->device_ids[2];
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(g_sixcam.jhh02_id, &info);
        printf("  %-12s -> device[%u] %s  4000x1200@30 IMU=Y (SixCam)\n",
               "jhh02", g_sixcam.jhh02_id, info.device);
    } else if (g_sixcam.enabled) {
        fprintf(stderr, "WARN: jhh02 not found\n");
        g_sixcam.enabled = false;
    }

    // 统计启用的设备数
    int active = 0;
    for (int i = 0; i < N_CAMS; i++) if (CAMS[i].enabled) active++;
    if (g_sixcam.enabled) active += 2;

    return active;
}
```

同时更新 `SixCamEntry` 结构体 (Line 87-92)：

```cpp
struct SixCamEntry {
    bool enabled = true;
    uint32_t jhh04_id = 0;   // ★ 旧: v4l2_dev_sys_data_t*
    uint32_t jhh02_id = 0;
};
```

和 `CamEntry` (Line 76-80)：

```cpp
struct CamEntry {
    CameraConfig cfg;
    bool enabled = true;
    // ★ 不再需要 dev_ptr
};
```

**Step 4: 更新 run_session() — Lines 291-374**

在两个地方更新：

a) 创建 VideoSensor 时传 device_id 而非 dev_ptr (Line 313)：

```cpp
auto* vs = new VideoSensor(cam.cfg, ses_dir, cam.cfg.device_id, session_num, session_ts, g_session_running);
```

b) 创建 SixCamSensor 时传 device IDs (Lines 318-327)：

```cpp
if (g_sixcam.enabled && g_sixcam.jhh04_id && g_sixcam.jhh02_id) {
    CameraConfig j04{...};
    CameraConfig j02{...};
    auto* sc = new SixCamSensor(j04, j02,
        g_sixcam.jhh04_id, g_sixcam.jhh02_id,
        ses_dir, session_num, session_ts, g_session_running);
    ...
}
```

**Step 5: 在 run_session 末尾添加 UnInit**

在 `run_session` 返回前（Line 373 之后）添加：

```cpp
    // ★ 每个 Session 结束后 UnInit SDK, 下次重新 Init (清除内部状态)
    fprintf(stderr, "DBG: Nori_Xvision_UnInit...\n");
    uint32_t ur = Nori_Xvision_UnInit();
    fprintf(stderr, "DBG: Nori_Xvision_UnInit done (0x%x)\n", ur);
```

**Step 6: 更新 scan_devices()**

将 `scan_devices` (Lines 121-136) 改为使用新 SDK 枚举（或者暂时移除，因为 Init 必须在 UnInit 后配对）：

```cpp
static void scan_devices() {
    uint32_t count = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &count);
    if (ret != NORI_OK) {
        printf("Nori_Xvision_Init failed: 0x%x\n", ret);
        return;
    }
    printf("Found %u device(s):\n", count);
    for (uint32_t i = 0; i < count; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);
        printf("  [%u] %04x:%04x \"%s\" \"%s\" %s\n",
               i, info.idVendor, info.idProduct,
               info.iManufacturer, info.iProduct, info.device);
        VERSION_INFO ver;
        if (Nori_Xvision_GetVersion(i, &ver) == NORI_OK) {
            printf("       SDK:%s  Type:%s\n", ver.SDKVersion, ver.DeviceType);
        }
    }
    Nori_Xvision_UnInit();
}
```

---

### Task 7: 编译验证

**Files:** 无新文件

**Step 1: 在板端编译**

```bash
ssh root@192.168.100.200 "cd ~/unified_capture && make clean && make 2>&1"
```

Expected: 编译成功，无错误。

**Step 2: 解决可能的编译错误**

常见问题：
- `camera_config.h` 的 initializer list 中 `device_id` 需赋默认值 `-1`
- `DEVICE_INFO` 的 `iProduct`/`iManufacturer` 是 `unsigned char[MAX_PATH]`，printf 用 `%s` 需要 cast
- `SixCamSensor.h` 的构造函数不再需要 `v4l2_dev_sys_data_t`

---

### Task 8: 功能验证 — 单 Session 采集测试

**Files:** 无

**Step 1: 单次采集测试**

```bash
ssh root@192.168.100.200 "cd ~/unified_capture && timeout 30 ./unified_capture --no-gpio --no-vive /data/capture 2>&1 | head -100"
```

Expected:
- Init OK, 找到 4 个设备
- 所有 camera setup OK
- 帧正常采集（GetFrameBuff 返回有效帧）
- VideoStop → DeviceVideoUnInit 正常
- Ctrl-C 后正常退出

**Step 2: 检查输出文件**

```bash
ssh root@192.168.100.200 "ls -la /data/capture/session_001/*/ && echo '---' && find /data/capture/session_001 -name '*.mkv' -exec ls -lh {} \;"
```

Expected: 每个通道有 MKV 和/或 Y8 文件，文件大小 > 0。

---

### Task 9: 功能验证 — 多 Session 采集测试

**Files:** 无

**Step 1: Socket 模式连续 3 次 Session**

```bash
ssh root@192.168.100.200 "cd ~/unified_capture && timeout 60 ./unified_capture --socket --single /data/capture 2>&1 &
sleep 2
echo start | nc -U /tmp/unified_capture.sock
sleep 8
echo start | nc -U /tmp/unified_capture.sock
sleep 8
echo start | nc -U /tmp/unified_capture.sock
sleep 8
wait"
```

Expected:
- 3 个 Session 全部正常启动/停止
- 每次 Init/UnInit 成功
- 无死锁、无崩溃
- 输出文件在 session_001、session_002、session_003 下

---

### Task 10: 清理和文档更新

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/bugs/2026-07-26-tstc-sdk-multi-session-issues.md` 状态更新

**Step 1: 更新 CLAUDE.md**

- 将 TSTC SDK 依赖改为 Nori Xvision SDK v10.00.09
- 更新编译命令中的路径
- 移除 `--single` + systemd 作为多 session bug 规避方案的说明（新 SDK 不再需要）
- 更新架构图中的 TSTC SDK → Nori Xvision SDK
- 更新全局互斥锁相关说明

**Step 2: 更新 Bug 报告状态**

将 bug 状态从 "已规避" 改为 "已解决（新 SDK v10.00.09）"。

**Step 3: Git commit**

```bash
git add -A
git commit -m "feat: migrate from TSTC USBCam_API to Nori Xvision SDK v10.00.09

- Replace all TSTC API calls with Nori Xvision equivalents
- Remove stream thread (no more DEAL_WITH event loop)
- Remove global stream start mutex (new SDK supports concurrent starts)
- Add SDK-level Init/UnInit boundary per session
- Preserve IMU init order (jhh02 before jhh04)
- Update Makefile for new include/lib paths

Fixes: multi-session deadlock (2026-07-26-tstc-sdk-multi-session-issues)"
```

---

### 回退方案

如果新 SDK 出现问题，回退步骤：
1. `git checkout` 恢复旧代码
2. 编译旧版: `make clean && make`
3. 旧 SDK 仍在 `/usr/local/TSTC/`，不受影响

---

### 关键设计决策

| 决策 | 说明 |
|------|------|
| 使用 GetFrameBuff 轮询而非回调 | 保持与旧代码相同的 pull 模型，最小化 collect 循环改动 |
| 保留 g_jhh2_remaining / g_jhh02_init_done | IMU 启流顺序是硬件约束（jhh04 IMU 数据通过 jhh02 通道），不受 SDK 更换影响 |
| 移除 g_stream_start_mutex | 新 SDK 支持 4 路设备并发启动（已验证），不再需要串行锁 |
| 移除 stream_thread | 新 SDK 无 DEAL_WITH 事件循环，VideoStart 后 SDK 内部管理取流 |
| 每 Session Init + UnInit | 利用新 SDK 的边界清除内部状态，解决旧 SDK 的多 Session 死锁 |
| 设备匹配按 VID/PID + 枚举顺序 | 新 SDK 的 Init 枚举稳定，4 个设备的 group_order 映射正确 |
