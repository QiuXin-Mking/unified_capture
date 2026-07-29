#pragma once
/*
 * video_sensor.h — VideoSensor: V4L2 采集 + MPP H.265 编码 + FFmpeg MKV 封装
 *
 * 每个 VideoSensor 对应一个摄像头, 内部包含:
 *   - V4L2 帧轮询 (非阻塞 DQBUF)
 *   - turbojpeg MJPEG → BGR 解码
 *   - MPP H.265 硬件编码
 *   - FFmpeg 子进程 (FIFO → MKV)
 *   - FrameQueue: BGR 帧 → ImuSensor 异步消费
 */

#include "hardware/common/sensor.h"

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

// V4L2 device
#include "hardware/video/v4l2_device.h"

// elapsed_us() / g_t0 定义在 sensor.h

// ============================================================
#include "hardware/video/bgr2nv12.h"

#include "hardware/video/mpp_encoder.h"
#include "hardware/video/capture_control.h"
#include "core/camera_config.h"

class VideoSensor : public Sensor {
public:
    VideoSensor(const CameraConfig& cfg,
                const std::string& session_dir,
                const std::string& device_path,
                int session_num,
                const std::string& session_ts,
                std::atomic<bool>& running,
                VideoCaptureControl& control)
        : Sensor(cfg.name, running)
        , cfg_(cfg)
        , session_dir_(session_dir)
        , device_path_(device_path)
        , session_num_(session_num)
        , session_ts_(session_ts)
        , control_(control) {}

    ~VideoSensor() override = default;

    FrameQueue& imu_queue() { return imu_queue_; }

protected:
    void setup() override {
        char path[256];
        fprintf(stderr, "[%s] DBG setup: ENTER (device=%s)\n", cfg_.name, device_path_.c_str());

        // ── 创建输出目录 ──
        snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), cfg_.name);
        out_dir_ = path;
        mkdir_p(out_dir_.c_str(), 0755);

        // ── 1. V4L2 初始化采集 ──
        fprintf(stderr, "[%s] DBG setup: V4L2 open %s %dx%d@%d...\n",
                cfg_.name, device_path_.c_str(), cfg_.width, cfg_.height, cfg_.fps);
        if (!device_.open(device_path_, cfg_.width, cfg_.height, cfg_.fps)) {
            fprintf(stderr, "[%s] V4L2 open failed\n", cfg_.name);
            return;
        }
        fprintf(stderr, "[%s] DBG setup: V4L2 open OK\n", cfg_.name);

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
                snprintf(path, sizeof(path), "%s/%s-%s.mkv",
                         out_dir_.c_str(), cfg_.name, session_ts_.c_str());
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
            snprintf(path, sizeof(path), "%s/%s-%s.y8",
                     out_dir_.c_str(), cfg_.name, session_ts_.c_str());
            y8_fp_ = fopen(path, "w");
            if (!y8_fp_) {
                fprintf(stderr, "[%s] cannot create Y8 file %s\n", cfg_.name, path);
            } else {
                fprintf(stderr, "[%s] DBG setup: Y8 file %s opened\n", cfg_.name, path);
            }
        }

        // ★ 等六目 jhh02 先完成启流 (IMU 硬件依赖)
        fprintf(stderr, "[%s] DBG setup: jhh02_init_done=%d, waiting...\n",
                cfg_.name, (int)control_.jhh02_init_done);
        for (int wait_i = 0; wait_i < 500 && !control_.jhh02_init_done; wait_i++) {
            usleep(20000);  // 20ms × 500 = 10s timeout
        }
        fprintf(stderr, "[%s] DBG setup: wait done, jhh02_init_done=%d\n",
                cfg_.name, (int)control_.jhh02_init_done);

        // ── 5. 启动视频流 ──
        fprintf(stderr, "[%s] DBG setup: start_stream...\n", cfg_.name);
        if (!device_.start_stream()) {
            fprintf(stderr, "[%s] start_stream failed\n", cfg_.name);
            return;
        }
        fprintf(stderr, "[%s] DBG setup: start_stream OK\n", cfg_.name);

        if (cfg_.vid == 0x1bcf && cfg_.pid == 0x2d50) {
            int rem = --control_.jhh2_remaining;
            fprintf(stderr, "[%s] DBG: JHH2 done, remaining=%d\n", cfg_.name, rem);
        }

        printf("[%s] setup OK  (device=%s, %dx%d@%dfps, H265=%c, Y8=%c)\n",
               cfg_.name, device_path_.c_str(), cfg_.width, cfg_.height, cfg_.fps,
               cfg_.output_h265 ? 'Y' : 'N', cfg_.output_y8 ? 'Y' : 'N');
        fprintf(stderr, "[%s] DBG setup: EXIT (initialized=true)\n", cfg_.name);
        initialized_ = true;
    }

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

        // ★ nv12 延迟分配: 按实际 JPEG 尺寸, 不是配置尺寸
        uint32_t nv12_size = 0;
        uint8_t* nv12 = nullptr;
        int nv12_w = 0, nv12_h = 0;

        fprintf(stderr, "[%s] DBG collect: entering frame loop (running=%d)\n",
                cfg_.name, (int)running_);

        while (running_) {
            size_t mjpg_len = 0;
            uint8_t* mjpg = device_.dequeue_frame(mjpg_len);
            if (!mjpg) {
                empty_polls++;
                if (empty_polls == 1) {
                    fprintf(stderr, "[%s] DBG collect: dequeue NULL (first)\n", cfg_.name);
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

            // --- MJPEG → BGR (turbojpeg) ---
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000 || h <= 0 || h > 8000) {
                if (frame_idx == 0) {
                    fprintf(stderr, "[%s] DBG collect: tjDecompressHeader2 failed, len=%zu\n",
                            cfg_.name, mjpg_len);
                }
                device_.requeue_frame();
                continue;
            }

            // ★ 按实际 JPEG 尺寸分配 nv12 (第一帧时分配)
            // 注意: MPP 要求 NV12 行跨度 64 字节对齐
            if (!nv12 || w != nv12_w || h != nv12_h) {
                delete[] nv12;
                nv12_w = w; nv12_h = h;
                nv12_stride_ = (w + 63) & ~63;
                nv12_size = nv12_stride_ * h * 3 / 2;
                nv12 = new (std::nothrow) uint8_t[nv12_size];
                if (!nv12) {
                    fprintf(stderr, "[%s] FATAL: nv12 alloc failed (w=%d h=%d size=%u)\n",
                            cfg_.name, w, h, nv12_size);
                    device_.requeue_frame();
                    break;
                }
                fprintf(stderr, "[%s] actual resolution %dx%d (cfg=%dx%d) stride=%d\n",
                        cfg_.name, w, h, cfg_.width, cfg_.height, nv12_stride_);
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
                bgr_to_nv12(bgr, w, h, nv12_stride_, nv12);

                if (cfg_.output_h265) {
                    size_t h265_bytes = mpp_.put(nv12, fifo_fp_);
                    total_h265 += h265_bytes;
                }

                if (cfg_.output_y8 && y8_fp_) {
                    fwrite(nv12, 1, w * h, y8_fp_);  // ★ 按实际尺寸写 Y8
                }

                // Preview JPEG export (on-demand, in collect thread — no extra pthread)
                std::string preview_path;
                if (control_.take_preview(preview_path)) {
                    // Downscale to 1/4 resolution for 5.5" screen preview
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
                    // JPEG compress (libjpeg-turbo, already linked)
                    unsigned long jpg_size = 0;
                    uint8_t* jpg_buf = nullptr;
                    tjhandle jpg_h = tjInitCompress();
                    if (jpg_h) {
                        int jr = tjCompress2(jpg_h, scaled.data(), pw, 0, ph,
                                             TJPF_BGR, &jpg_buf, &jpg_size,
                                             TJSAMP_420, 85, TJFLAG_FASTDCT);
                        if (jr == 0 && jpg_buf && jpg_size > 0) {
                            // Atomic write: temp file → rename
                            std::string tmp = preview_path + ".tmp";
                            FILE* fp = fopen(tmp.c_str(), "wb");
                            if (fp) {
                                fwrite(jpg_buf, 1, jpg_size, fp);
                                fclose(fp);
                                rename(tmp.c_str(), preview_path.c_str());
                            }
                            tjFree(jpg_buf);
                        }
                        tjDestroy(jpg_h);
                    }
                }
            } else if (frame_idx == 0) {
                fprintf(stderr, "[%s] DBG collect: tjDecompress2 failed, ret=%d, w=%d h=%d len=%zu\n",
                        cfg_.name, dec_ret, w, h, mjpg_len);
            }

            delete[] bgr;
            device_.requeue_frame();
            frame_idx++;

            if (frame_idx % 30 == 0) {
                fprintf(stderr, "[%s] DBG collect: frame=%llu, last_mjpg_len=%zu, h265_total=%zu\n",
                        cfg_.name, (unsigned long long)frame_idx, mjpg_len, total_h265);
            }
        }

        fprintf(stderr, "[%s] DBG collect: loop exit (frame_idx=%llu, empty_polls=%d)\n",
                cfg_.name, (unsigned long long)frame_idx, empty_polls);

        // 停止流
        fprintf(stderr, "[%s] DBG collect: stop_stream...\n", cfg_.name);
        device_.stop_stream();

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

        // 关闭 V4L2 设备
        fprintf(stderr, "[%s] DBG teardown: V4L2 close...\n", cfg_.name);
        device_.close();

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
    std::string device_path_;
    V4l2Device device_;
    int session_num_;
    std::string session_ts_;
    VideoCaptureControl& control_;

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

    // NV12 stride (MPP 要求 64 字节对齐)
    int nv12_stride_ = 0;

    bool initialized_ = false;
};
