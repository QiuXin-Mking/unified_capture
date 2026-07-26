#pragma once
/*
 * video_sensor.h — VideoSensor: TSTC SDK 采集 + MPP H.265 编码 + FFmpeg MKV 封装
 *
 * 每个 VideoSensor 对应一个摄像头, 内部包含:
 *   - TSTC SDK 采集线程 (vendor SDK 事件循环)
 *   - turbojpeg MJPEG → BGR 解码
 *   - MPP H.265 硬件编码
 *   - FFmpeg 子进程 (FIFO → MKV)
 *   - FrameQueue: BGR 帧 → ImuSensor 异步消费
 */

#include "sensor.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>

#include <turbojpeg.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

// TSTC SDK
#include "USBCam_API.h"

// elapsed_us() / g_t0 定义在 sensor.h

// ============================================================
#include "bgr2nv12.h"

#include "mpp_encoder.h"
#include "camera_config.h"

// ============================================================
// 全局互斥锁: TSTC SDK 的 STREAM_STATUS 不支持多路并发调用,
// 必须串行化 (否则在多个 JHH2 设备上同时调用会死锁)
// ============================================================
static std::mutex g_stream_start_mutex;
extern std::atomic<int> g_jhh2_remaining;  // jhh04 等待此计数器归零

// Preview JPEG export globals (defined in main.cpp)
extern std::atomic<bool> g_preview_pending;
extern std::string g_preview_path;
extern std::mutex g_preview_mutex;

class VideoSensor : public Sensor {
public:
    VideoSensor(const CameraConfig& cfg,
                const std::string& session_dir,
                v4l2_dev_sys_data_t& dev_info,
                int session_num,
                std::atomic<bool>& running)
        : Sensor(cfg.name, running)
        , cfg_(cfg)
        , session_dir_(session_dir)
        , dev_info_(dev_info)
        , session_num_(session_num) {}

    ~VideoSensor() override = default;

    FrameQueue& imu_queue() { return imu_queue_; }

protected:
    void setup() override {
        char path[256];
        fprintf(stderr, "[%s] DBG setup: ENTER\n", cfg_.name);

        // ── 创建输出目录 ──
        snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), cfg_.name);
        out_dir_ = path;
        mkdir_p(out_dir_.c_str(), 0755);

        // ── 1. TSTC SDK 初始化 (全局锁保护, 避免同 VID/PID 设备冲突) ──
        fprintf(stderr, "[%s] DBG setup: creating device point...\n", cfg_.name);
        tstc_handle_ = TST_USBCam_CREATE_DEVICE_POINT(dev_info_);
        if (!tstc_handle_) {
            fprintf(stderr, "[%s] TST_USBCam_CREATE_DEVICE_POINT failed\n", cfg_.name);
            return;
        }
        fprintf(stderr, "[%s] DBG setup: handle=%p\n", cfg_.name, tstc_handle_);

        fprintf(stderr, "[%s] DBG setup: opening %s...\n", cfg_.name, dev_info_.Device_Path);
        dev_fd_ = open(dev_info_.Device_Path, O_RDWR | O_NONBLOCK);
        if (dev_fd_ < 0) {
            fprintf(stderr, "[%s] open %s failed: %s\n", cfg_.name, dev_info_.Device_Path, strerror(errno));
            return;
        }
        fprintf(stderr, "[%s] DBG setup: dev_fd=%d\n", cfg_.name, dev_fd_);


        // ── 2. MPP 编码器 ──
        if (cfg_.output_h265) {
            fprintf(stderr, "[%s] DBG setup: MPP init %dx%d...\n", cfg_.name, cfg_.width, cfg_.height);
            if (!mpp_.init(cfg_.width, cfg_.height, cfg_.bitrate, cfg_.fps, cfg_.gop)) {
                fprintf(stderr, "[%s] MPP init failed\n", cfg_.name);
                return;
            }
            fprintf(stderr, "[%s] DBG setup: MPP init OK\n", cfg_.name);
        }

        // ── 3. FIFO + FFmpeg MKV 封装 ──
        if (cfg_.output_h265) {
            snprintf(path, sizeof(path), "/tmp/h265_%s_fifo", cfg_.name);
            fifo_path_ = path;
            unlink(fifo_path_.c_str());
            if (mkfifo(fifo_path_.c_str(), 0666) < 0) {
                perror("mkfifo");
                return;
            }
            fprintf(stderr, "[%s] DBG setup: fifo=%s created\n", cfg_.name, fifo_path_.c_str());

            ffmpeg_pid_ = fork();
            if (ffmpeg_pid_ < 0) {
                perror("fork ffmpeg");
                return;
            }
            if (ffmpeg_pid_ == 0) {
                // 子进程: ffmpeg 读 FIFO → MKV
                snprintf(path, sizeof(path), "%s/%03d.mkv",
                         out_dir_.c_str(), session_num_);
                char fps_s[16];
                snprintf(fps_s, sizeof(fps_s), "%d", cfg_.fps);
                execlp("ffmpeg", "ffmpeg",
                       "-y", "-hide_banner", "-loglevel", "error",
                       "-f", "hevc", "-r", fps_s,
                       "-i", fifo_path_.c_str(),
                       "-c", "copy",
                       path, NULL);
                perror("exec ffmpeg");
                _exit(1);
            }
            fprintf(stderr, "[%s] DBG setup: ffmpeg pid=%d\n", cfg_.name, ffmpeg_pid_);

            // 打开 FIFO 写入端 (阻塞直到 ffmpeg 打开读端)
            fprintf(stderr, "[%s] DBG setup: opening fifo write end (may block)...\n", cfg_.name);
            fifo_fd_ = open(fifo_path_.c_str(), O_WRONLY);
            if (fifo_fd_ < 0) {
                perror("open fifo for write");
                return;
            }
            fifo_fp_ = fdopen(fifo_fd_, "w");
            fprintf(stderr, "[%s] DBG setup: fifo write end opened\n", cfg_.name);
        }

        // ── 4. Y8 原始灰度文件 ──
        if (cfg_.output_y8) {
            snprintf(path, sizeof(path), "%s/%03d.y8",
                     out_dir_.c_str(), session_num_);
            y8_fp_ = fopen(path, "w");
            if (!y8_fp_) {
                fprintf(stderr, "[%s] cannot create Y8 file %s\n", cfg_.name, path);
            } else {
                fprintf(stderr, "[%s] DBG setup: Y8 file %s opened\n", cfg_.name, path);
            }
        }

        // ★ 整段上锁: DEAL_WITH_INIT → stream线程 → STREAM_STATUS
        //    同 VID/PID 设备必须完全串行, 否则 SDK 内部状态冲突阻塞
        fprintf(stderr, "[%s] DBG setup: acquiring stream_start_mutex...\n", cfg_.name);
        {
            std::lock_guard<std::mutex> lock(g_stream_start_mutex);
            fprintf(stderr, "[%s] DBG setup: LOCKED\n", cfg_.name);

            fprintf(stderr, "[%s] DBG setup: Video_DEAL_WITH_INIT...\n", cfg_.name);
            if (TST_USBCam_Video_DEAL_WITH_INIT(tstc_handle_, dev_fd_) != 0) {
                fprintf(stderr, "[%s] Video_DEAL_WITH_INIT failed\n", cfg_.name);
                return;
            }
            fprintf(stderr, "[%s] DBG setup: Video_DEAL_WITH_INIT OK\n", cfg_.name);

            fprintf(stderr, "[%s] DBG setup: creating stream thread...\n", cfg_.name);
            pthread_create(&stream_thread_, nullptr, VideoSensor::stream_thread_func, this);
            usleep(200000);
            fprintf(stderr, "[%s] DBG setup: calling STREAM_STATUS(1)...\n", cfg_.name);
            TST_USBCam_Video_STREAM_STATUS(tstc_handle_, 1);
            fprintf(stderr, "[%s] DBG setup: STREAM_STATUS(1) done\n", cfg_.name);
            if (cfg_.vid == 0x1bcf && cfg_.pid == 0x2d50) {
                int rem = --g_jhh2_remaining;
                fprintf(stderr, "[%s] DBG: JHH2 done, remaining=%d\n", cfg_.name, rem);
            }
        }
        fprintf(stderr, "[%s] DBG setup: stream_start_mutex released\n", cfg_.name);

        printf("[%s] setup OK  (dev=%s, %dx%d@%dfps, H265=%c, Y8=%c)\n",
               cfg_.name, dev_info_.Device_Path, cfg_.width, cfg_.height, cfg_.fps,
               cfg_.output_h265 ? 'Y' : 'N', cfg_.output_y8 ? 'Y' : 'N');
        fprintf(stderr, "[%s] DBG setup: EXIT (initialized=true)\n", cfg_.name);
        initialized_ = true;
    }

    void collect() override {
        if (!initialized_) {
            fprintf(stderr, "[%s] not initialized, skip\n", cfg_.name);
            return;
        }

        fprintf(stderr, "[%s] DBG collect: ENTER (stream already started in setup)\n", cfg_.name);

        tjhandle tj = tjInitDecompress();
        uint64_t frame_idx = 0;
        size_t total_h265 = 0;
        int empty_polls = 0;

        // ★ nv12 延迟分配: 按实际 JPEG 尺寸, 不是配置尺寸
        uint32_t nv12_size = 0;
        uint8_t* nv12 = nullptr;
        int nv12_w = 0, nv12_h = 0;

        fprintf(stderr, "[%s] DBG collect: entering frame loop (running=%d)\n",
                cfg_.name, (int)running_);

        while (running_) {
            Frame_Buffer_Data* fb = TST_USBCam_GET_FRAME_BUFF(tstc_handle_, 0);
            if (!fb) {
                empty_polls++;
                if (empty_polls == 1) {
                    fprintf(stderr, "[%s] DBG collect: GET_FRAME_BUFF returned NULL (first time)\n", cfg_.name);
                }
                usleep(1000);
                continue;
            }

            if (empty_polls > 0) {
                fprintf(stderr, "[%s] DBG collect: got first frame after %d empty polls\n",
                        cfg_.name, empty_polls);
                empty_polls = 0;
            }

            uint64_t ts_us = elapsed_us();
            uint8_t* mjpg = (uint8_t*)fb->pMem;
            size_t mjpg_len = fb->buffer.bytesused;

            // --- MJPEG → BGR (turbojpeg) ---
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000 || h <= 0 || h > 8000) {
                if (frame_idx == 0) {
                    fprintf(stderr, "[%s] DBG collect: tjDecompressHeader2 failed, len=%zu\n",
                            cfg_.name, mjpg_len);
                }
                TST_USBCam_SAVE_FRAME_RES(tstc_handle_, fb);
                continue;
            }

            // ★ 按实际 JPEG 尺寸分配 nv12 (第一帧时分配)
            if (!nv12 || w != nv12_w || h != nv12_h) {
                delete[] nv12;
                nv12_w = w; nv12_h = h;
                nv12_size = w * h * 3 / 2;
                nv12 = new (std::nothrow) uint8_t[nv12_size];
                if (!nv12) {
                    fprintf(stderr, "[%s] FATAL: nv12 alloc failed (w=%d h=%d size=%u)\n",
                            cfg_.name, w, h, nv12_size);
                    TST_USBCam_SAVE_FRAME_RES(tstc_handle_, fb);
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
                // → BGR 推送到 IMU 队列 (非阻塞, 满了就丢)
                if (cfg_.has_imu) {
                    BGRFrame imu_frame(frame_idx, ts_us, w, h);
                    memcpy(imu_frame.data.data(), bgr, bgr_size);
                    imu_queue_.try_push(std::move(imu_frame));
                }

                // → BGR → NV12 → MPP H.265 → FIFO + Y8
                bgr_to_nv12(bgr, w, h, nv12);

                if (cfg_.output_h265) {
                    size_t h265_bytes = mpp_.put(nv12, fifo_fp_);
                    total_h265 += h265_bytes;
                }

                if (cfg_.output_y8 && y8_fp_) {
                    fwrite(nv12, 1, w * h, y8_fp_);  // ★ 按实际尺寸写 Y8
                }

                // Preview JPEG export (on-demand, in collect thread — no extra pthread)
                if (g_preview_pending.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(g_preview_mutex);
                    if (g_preview_pending.load()) {
                        // Downscale to 1/4 resolution for 5.5" screen preview
                        int pw = w / 4, ph = h / 4;
                        if (pw < 1) pw = 1; if (ph < 1) ph = 1;
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
                        // JPEG compress (libjpeg-turbo, already linked)
                        unsigned long jpg_size = 0;
                        uint8_t* jpg_buf = nullptr;
                        tjhandle jpg_h = tjInitCompress();
                        if (jpg_h) {
                            int jr = tjCompress2(jpg_h, scaled.data(), pw, ph, 0,
                                                 TJPF_BGR, &jpg_buf, &jpg_size,
                                                 TJSAMP_444, 85, TJFLAG_FASTDCT);
                            if (jr == 0 && jpg_buf && jpg_size > 0) {
                                // Atomic write: temp file → rename
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
                fprintf(stderr, "[%s] DBG collect: tjDecompress2 failed, ret=%d, w=%d h=%d len=%zu\n",
                        cfg_.name, dec_ret, w, h, mjpg_len);
            }

            delete[] bgr;
            TST_USBCam_SAVE_FRAME_RES(tstc_handle_, fb);
            frame_idx++;

            if (frame_idx % 30 == 0) {
                fprintf(stderr, "[%s] DBG collect: frame=%llu, last_mjpg_len=%zu, h265_total=%zu\n",
                        cfg_.name, (unsigned long long)frame_idx, mjpg_len, total_h265);
            }
        }

        fprintf(stderr, "[%s] DBG collect: loop exit (frame_idx=%llu, empty_polls=%d)\n",
                cfg_.name, (unsigned long long)frame_idx, empty_polls);

        // 停止流
        fprintf(stderr, "[%s] DBG collect: stopping TSTC stream...\n", cfg_.name);
        TST_USBCam_EVENT_LoopMode(tstc_handle_, 0);
        TST_USBCam_Video_STREAM_STATUS(tstc_handle_, 0);  // 配对 STREAM_STATUS(1) in setup

        // Flush MPP
        if (cfg_.output_h265) {
            printf("[%s] flushing encoder (%zu frames, %.1f MB H.265)...\n",
                   cfg_.name, frame_idx, total_h265 / 1048576.0);
            fprintf(stderr, "[%s] DBG collect: MPP flush...\n", cfg_.name);
            mpp_.flush(fifo_fp_);
            fprintf(stderr, "[%s] DBG collect: MPP flush done\n", cfg_.name);
        } else {
            printf("[%s] stopped (%zu frames)\n", cfg_.name, frame_idx);
        }

        delete[] nv12;
        tjDestroy(tj);
        fprintf(stderr, "[%s] DBG collect: EXIT\n", cfg_.name);
    }

    void teardown() override {
        fprintf(stderr, "[%s] DBG teardown: ENTER\n", cfg_.name);

        if (cfg_.output_h265) {
            if (fifo_fp_) {
                fprintf(stderr, "[%s] DBG teardown: closing fifo_fp...\n", cfg_.name);
                fclose(fifo_fp_); fifo_fp_ = nullptr; fifo_fd_ = -1;
                fprintf(stderr, "[%s] DBG teardown: fifo_fp closed\n", cfg_.name);
            }

            // 等待 ffmpeg 退出 (FIFO 读端收到 EOF)
            if (ffmpeg_pid_ > 0) {
                fprintf(stderr, "[%s] DBG teardown: waiting for ffmpeg pid=%d...\n",
                        cfg_.name, ffmpeg_pid_);
                int status;
                waitpid(ffmpeg_pid_, &status, 0);
                printf("[%s] ffmpeg exited (%d)\n", cfg_.name, WEXITSTATUS(status));
                fprintf(stderr, "[%s] DBG teardown: ffmpeg wait done\n", cfg_.name);
            }
        }

        // 等待 TSTC 流线程
        if (stream_thread_) {
            fprintf(stderr, "[%s] DBG teardown: joining stream thread...\n", cfg_.name);
            pthread_join(stream_thread_, nullptr);
            fprintf(stderr, "[%s] DBG teardown: stream thread joined\n", cfg_.name);
        }

        // 清理 MPP
        if (cfg_.output_h265) {
            fprintf(stderr, "[%s] DBG teardown: destroying MPP...\n", cfg_.name);
            mpp_.destroy();
            fprintf(stderr, "[%s] DBG teardown: MPP destroyed\n", cfg_.name);
        }

        // 清理 FIFO
        if (cfg_.output_h265) {
            fprintf(stderr, "[%s] DBG teardown: unlinking fifo...\n", cfg_.name);
            unlink(fifo_path_.c_str());
        }

        // 关闭 Y8 文件
        if (y8_fp_) { fclose(y8_fp_); y8_fp_ = nullptr; }

        printf("[%s] teardown OK\n", cfg_.name);
        fprintf(stderr, "[%s] DBG teardown: EXIT\n", cfg_.name);
    }

    const CameraConfig& config() const { return cfg_; }

private:
    CameraConfig cfg_;
    std::string session_dir_;
    std::string out_dir_;
    v4l2_dev_sys_data_t& dev_info_;
    int session_num_;

    // TSTC
    void* tstc_handle_ = nullptr;
    int dev_fd_ = -1;
    pthread_t stream_thread_ = 0;

    // MPP
    MppEncoder mpp_;

    // FFmpeg
    pid_t ffmpeg_pid_ = 0;
    std::string fifo_path_;
    int fifo_fd_ = -1;
    FILE* fifo_fp_ = nullptr;

    // Y8
    FILE* y8_fp_ = nullptr;

    // IMU queue
    FrameQueue imu_queue_{4};

    bool initialized_ = false;

    // ---- TSTC SDK 内部流线程 ----
    static void* stream_thread_func(void* arg) {
        auto* self = (VideoSensor*)arg;
        Pix_Format fmt;
        fmt.u_PixFormat = 0;
        fmt.u_Width  = (uint32_t)self->cfg_.width;
        fmt.u_Height = (uint32_t)self->cfg_.height;
        fmt.u_Fps    = (uint32_t)self->cfg_.fps;
        TST_USBCam_Video_DEAL_WITH(self->tstc_handle_, fmt);
        TST_USBCam_Video_DEAL_WITH_UNINIT(self->tstc_handle_);
        TST_USBCam_DELETE_DEVICE_POINT(self->tstc_handle_);
        return nullptr;
    }
};

