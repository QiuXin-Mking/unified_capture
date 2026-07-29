#pragma once
/*
 * sixcam_sensor.h — SixCamSensor: 六目模组双通道一体化采集
 *
 * 六目模组是一个物理主板, 通过 USB 暴露两个通道:
 *   JHH04 (四目): VID/PID 1bcf:2d51, 3104×480@30, 仅 Y8
 *   JHH02 (双目): VID/PID 1bcf:2d50, 4000×1200@30, H.265 + Y8
 *
 * 启流顺序: jhh02 必须优先于 jhh04 (IMU 硬件依赖)
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

#include "hardware/video/v4l2_device.h"

#include "hardware/video/bgr2nv12.h"
#include "hardware/video/capture_control.h"
#include "hardware/video/mpp_encoder.h"
#include "core/camera_config.h"
#include "hardware/imu/imu_decode.h"
#include "core/frame_queue.h"

// ============================================================
// 单个通道的内部状态
// ============================================================
struct SixCamChannel {
    const char* name;
    int width, height, fps, bitrate, gop;
    bool output_h265;       // false → 仅 Y8
    bool output_y8;
    bool has_imu;
    ImuOrientation imu_orientation;

    // V4L2
    std::string device_path;
    V4l2Device device;

    // MPP
    MppEncoder mpp;

    // FFmpeg
    pid_t      ffmpeg_pid = 0;
    std::string fifo_path;
    int         fifo_fd = -1;
    FILE*       fifo_fp = nullptr;

    // Y8
    FILE* y8_fp = nullptr;

    // IMU queue
    FrameQueue imu_queue{4};

    std::string out_dir;

    bool initialized = false;
};

// ============================================================
// SixCamSensor
// ============================================================
class SixCamSensor : public Sensor {
public:
    SixCamSensor(const CameraConfig& jhh04_cfg,
                 const CameraConfig& jhh02_cfg,
                 const std::string& jhh04_path,
                 const std::string& jhh02_path,
                 const std::string& session_dir,
                 int session_num,
                 const std::string& session_ts,
                 std::atomic<bool>& running,
                 VideoCaptureControl& control)
        : Sensor("sixcam", running)
        , session_dir_(session_dir)
        , session_num_(session_num)
        , session_ts_(session_ts)
        , control_(control)
    {
        ch_[0].name        = jhh04_cfg.name;
        ch_[0].width       = jhh04_cfg.width;
        ch_[0].height      = jhh04_cfg.height;
        ch_[0].fps         = jhh04_cfg.fps;
        ch_[0].bitrate     = jhh04_cfg.bitrate;
        ch_[0].gop         = jhh04_cfg.gop;
        ch_[0].output_h265 = jhh04_cfg.output_h265;
        ch_[0].output_y8   = jhh04_cfg.output_y8;
        ch_[0].has_imu     = jhh04_cfg.has_imu;
        ch_[0].imu_orientation = jhh04_cfg.imu_orientation;
        ch_[0].device_path = jhh04_path;

        ch_[1].name        = jhh02_cfg.name;
        ch_[1].width       = jhh02_cfg.width;
        ch_[1].height      = jhh02_cfg.height;
        ch_[1].fps         = jhh02_cfg.fps;
        ch_[1].bitrate     = jhh02_cfg.bitrate;
        ch_[1].gop         = jhh02_cfg.gop;
        ch_[1].output_h265 = jhh02_cfg.output_h265;
        ch_[1].output_y8   = jhh02_cfg.output_y8;
        ch_[1].has_imu     = jhh02_cfg.has_imu;
        ch_[1].imu_orientation = jhh02_cfg.imu_orientation;
        ch_[1].device_path = jhh02_path;
    }

    // 对外暴露两个 IMU 队列 (供 ImuSensor 消费)
    FrameQueue& imu_queue_jhh04() { return ch_[0].imu_queue; }
    FrameQueue& imu_queue_jhh02() { return ch_[1].imu_queue; }
    const char* ch_name(int i) const { return ch_[i].name; }

protected:
    void setup() override {
        char path[256];

        // ── 1. 创建输出目录 ──
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), ch_[i].name);
            ch_[i].out_dir = path;
            mkdir_p(path, 0755);
        }

        // ── 2. V4L2 open 两个通道 ──
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            fprintf(stderr, "[%s] DBG: V4L2 open %s %dx%d@%d...\n",
                    ch.name, ch.device_path.c_str(), ch.width, ch.height, ch.fps);
            if (!ch.device.open(ch.device_path, ch.width, ch.height, ch.fps)) {
                fprintf(stderr, "[%s] V4L2 open failed\n", ch.name);
                return;
            }
            fprintf(stderr, "[%s] DBG: V4L2 open OK\n", ch.name);
        }

        // ── 3. MPP 编码器 (仅 jhh02 需要) ──
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (ch.output_h265) {
                fprintf(stderr, "[%s] DBG: MPP init %dx%d...\n", ch.name, ch.width, ch.height);
                if (!ch.mpp.init(ch.width, ch.height, ch.bitrate, ch.fps, ch.gop)) {
                    fprintf(stderr, "[%s] MPP init failed\n", ch.name);
                    return;
                }
                fprintf(stderr, "[%s] DBG: MPP init OK\n", ch.name);
            }
        }

        // ── 4. FIFO + FFmpeg (仅 jhh02 需要) ──
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (!ch.output_h265) continue;

            snprintf(path, sizeof(path), "/tmp/h265_%s_fifo", ch.name);
            ch.fifo_path = path;
            unlink(ch.fifo_path.c_str());
            if (mkfifo(ch.fifo_path.c_str(), 0666) < 0) {
                perror("mkfifo");
                return;
            }

            ch.ffmpeg_pid = fork();
            if (ch.ffmpeg_pid < 0) { perror("fork ffmpeg"); return; }
            if (ch.ffmpeg_pid == 0) {
                snprintf(path, sizeof(path), "%s/%s-%s.mkv",
                         ch.out_dir.c_str(), ch.name, session_ts_.c_str());
                char fps_s[16];
                snprintf(fps_s, sizeof(fps_s), "%d", ch.fps);
                execlp("ffmpeg", "ffmpeg",
                       "-y", "-hide_banner", "-loglevel", "error",
                       "-f", "hevc", "-r", fps_s,
                       "-i", ch.fifo_path.c_str(),
                       "-c", "copy", path, NULL);
                perror("exec ffmpeg");
                _exit(1);
            }

            ch.fifo_fd = open(ch.fifo_path.c_str(), O_WRONLY);
            if (ch.fifo_fd < 0) { perror("open fifo write"); return; }
            ch.fifo_fp = fdopen(ch.fifo_fd, "w");
        }

        // ── 5. Y8 文件 ──
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (!ch.output_y8) continue;
            snprintf(path, sizeof(path), "%s/%s-%s.y8",
                     ch.out_dir.c_str(), ch.name, session_ts_.c_str());
            ch.y8_fp = fopen(path, "w");
        }

        // ── 6. JHH02 先启流 (VID/PID=1bcf:2d50, IMU 主通道) ──
        {
            auto& ch = ch_[1];
            fprintf(stderr, "[%s] DBG: start_stream (IMU master)...\n", ch.name);
            if (!ch.device.start_stream()) {
                fprintf(stderr, "[%s] start_stream failed\n", ch.name);
                return;
            }
            fprintf(stderr, "[%s] DBG: start_stream OK\n", ch.name);
            control_.jhh02_init_done = true;  // ★ 通知独立JHH2可以继续
            int rem = --control_.jhh2_remaining;
            fprintf(stderr, "[%s] DBG: JHH2 done, remaining=%d\n", ch.name, rem);
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }

        // ── 7. JHH04 后启流 (VID/PID=1bcf:2d51) ──
        {
            auto& ch = ch_[0];
            fprintf(stderr, "[%s] DBG: waiting for %d JHH2 devices...\n",
                    ch.name, (int)control_.jhh2_remaining);
            while (control_.jhh2_remaining > 0) {
                usleep(20000);
            }
            fprintf(stderr, "[%s] DBG: all JHH2 done, start_stream...\n", ch.name);
            if (!ch.device.start_stream()) {
                fprintf(stderr, "[%s] start_stream failed\n", ch.name);
                return;
            }
            fprintf(stderr, "[%s] DBG: start_stream OK\n", ch.name);
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }
    }

    void collect() override {
        // 为两个通道各自启动一个采集线程
        std::thread t0([this]() { collect_channel(0); });
        std::thread t1([this]() { collect_channel(1); });
        t0.join();
        t1.join();
    }

    void teardown() override {
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (!ch.initialized) continue;

            fprintf(stderr, "[%s] DBG teardown: stop_stream...\n", ch.name);
            ch.device.stop_stream();

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

            fprintf(stderr, "[%s] DBG teardown: V4L2 close...\n", ch.name);
            ch.device.close();

            printf("[%s] teardown OK\n", ch.name);
        }
    }

private:
    SixCamChannel ch_[2];
    std::string session_dir_;
    int session_num_;
    std::string session_ts_;
    VideoCaptureControl& control_;

    // ============================================================
    // 单个通道的采集循环
    // ============================================================
    void collect_channel(int idx) {
        auto& ch = ch_[idx];
        if (!ch.initialized) {
            fprintf(stderr, "[%s] not initialized, skip\n", ch.name);
            return;
        }

        fprintf(stderr, "[%s] DBG collect: ENTER\n", ch.name);

        tjhandle tj = tjInitDecompress();
        uint64_t frame_idx = 0;
        size_t total_h265 = 0;
        int empty_polls = 0;

        // ★ nv12 延迟分配: 按实际 JPEG 尺寸, 不是配置尺寸
        uint32_t nv12_size = 0;
        uint8_t* nv12 = nullptr;
        int nv12_w = 0, nv12_h = 0;

        fprintf(stderr, "[%s] DBG collect: entering frame loop (running=%d)\n",
                ch.name, (int)running_);

        while (running_) {
            size_t mjpg_len = 0;
            uint8_t* mjpg = ch.device.dequeue_frame(mjpg_len);
            if (!mjpg) {
                empty_polls++;
                if (empty_polls == 1) {
                    fprintf(stderr, "[%s] DBG collect: dequeue NULL (first)\n", ch.name);
                }
                usleep(1000);
                continue;
            }

            if (empty_polls > 0) {
                fprintf(stderr, "[%s] DBG collect: first frame after %d empty polls\n",
                        ch.name, empty_polls);
                empty_polls = 0;
            }

            uint64_t ts_us = elapsed_us();

            // MJPEG → BGR
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000 || h <= 0 || h > 8000) {
                ch.device.requeue_frame();
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
                            ch.name, w, h, nv12_size);
                    ch.device.requeue_frame();
                    break;
                }
                fprintf(stderr, "[%s] actual resolution %dx%d (cfg=%dx%d)\n",
                        ch.name, w, h, ch.width, ch.height);
            }

            uint32_t bgr_size = w * h * 3;
            uint8_t* bgr = new uint8_t[bgr_size];
            int dec_ret = tjDecompress2(tj, mjpg, mjpg_len, bgr, w, 0, h,
                                        TJPF_BGR, TJFLAG_FASTDCT);

            if (dec_ret == 0) {
                // → IMU 队列
                if (ch.has_imu) {
                    BGRFrame imu_frame(frame_idx, ts_us, w, h);
                    memcpy(imu_frame.data.data(), bgr, bgr_size);
                    ch.imu_queue.try_push(std::move(imu_frame));
                }

                // → NV12 → H.265 + Y8
                bgr_to_nv12(bgr, w, h, nv12);

                if (ch.output_h265) {
                    size_t h265_bytes = ch.mpp.put(nv12, ch.fifo_fp);
                    total_h265 += h265_bytes;
                }
                if (ch.output_y8 && ch.y8_fp) {
                    fwrite(nv12, 1, w * h, ch.y8_fp);
                }

                // Preview JPEG export (on-demand, only from color channel)
                std::string preview_path;
                if (ch.output_h265 && control_.take_preview(preview_path)) {
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
            }

            delete[] bgr;
            ch.device.requeue_frame();
            frame_idx++;

            if (frame_idx % 30 == 0) {
                fprintf(stderr, "[%s] DBG: frame=%llu, h265=%zu\n",
                        ch.name, (unsigned long long)frame_idx, total_h265);
            }
        }

        fprintf(stderr, "[%s] DBG collect: loop exit (frames=%llu)\n",
                ch.name, (unsigned long long)frame_idx);

        // Flush MPP
        if (ch.output_h265) {
            printf("[%s] flushing encoder (%zu frames, %.1f MB)...\n",
                   ch.name, frame_idx, total_h265 / 1048576.0);
            ch.mpp.flush(ch.fifo_fp);
        }

        delete[] nv12;
        tjDestroy(tj);
        fprintf(stderr, "[%s] DBG collect: EXIT\n", ch.name);
    }
};
