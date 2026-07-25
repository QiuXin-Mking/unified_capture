#pragma once
/*
 * sixcam_sensor.h — SixCamSensor: 六目模组双通道一体化采集
 *
 * 六目模组是一个物理主板, 通过 USB 暴露两个通道:
 *   JHH04 (四目): VID/PID 1bcf:2d51, 3104×480@30, 仅 Y8
 *   JHH02 (双目): VID/PID 1bcf:2d50, 3104×480@30, H.265 + Y8
 *
 * 两个通道必须在同一个线程中串行初始化/启流,
 * 否则 TSTC SDK 对同 VID/PID 设备的 STREAM_STATUS 会死锁.
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

#include "USBCam_API.h"

#include "bgr2nv12.h"
#include "mpp_encoder.h"
#include "camera_config.h"
#include "imu_decode.h"
#include "frame_queue.h"

// 复用 VideoSensor 的全局互斥锁 (定义在 video_sensor.h)
extern std::mutex g_stream_start_mutex;
extern std::atomic<int> g_jhh2_remaining;  // jhh04 等待此计数器归零

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

    // TSTC
    void*  tstc_handle = nullptr;
    int    dev_fd = -1;
    pthread_t stream_thread = 0;

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
                 v4l2_dev_sys_data_t& jhh04_dev,
                 v4l2_dev_sys_data_t& jhh02_dev,
                 const std::string& session_dir,
                 int session_num,
                 std::atomic<bool>& running)
        : Sensor("sixcam", running)
        , session_dir_(session_dir)
        , session_num_(session_num)
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
        ch_[0].dev_fd      = -1;

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
        ch_[1].dev_fd      = -1;

        // 保存设备信息
        jhh04_dev_ = jhh04_dev;
        jhh02_dev_ = jhh02_dev;
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

        // ── 2. 两个通道只做 CREATE + open (轻量, 不触发固件) ──
        //    DEAL_WITH_INIT 必须等 JHH2 独立相机完成后再做, 否则 IMU 全零
        v4l2_dev_sys_data_t* devs[2] = { &jhh04_dev_, &jhh02_dev_ };
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            fprintf(stderr, "[%s] DBG: creating device point...\n", ch.name);
            ch.tstc_handle = TST_USBCam_CREATE_DEVICE_POINT(*devs[i]);
            if (!ch.tstc_handle) {
                fprintf(stderr, "[%s] CREATE_DEVICE_POINT failed\n", ch.name);
                return;
            }
            ch.dev_fd = open(devs[i]->Device_Path, O_RDWR | O_NONBLOCK);
            if (ch.dev_fd < 0) {
                fprintf(stderr, "[%s] open %s failed\n", ch.name, devs[i]->Device_Path);
                return;
            }
            // DEAL_WITH_INIT 延迟到锁内
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
                snprintf(path, sizeof(path), "%s/%03d.mkv",
                         ch.out_dir.c_str(), session_num_);
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
            snprintf(path, sizeof(path), "%s/%03d.y8",
                     ch.out_dir.c_str(), session_num_);
            ch.y8_fp = fopen(path, "w");
        }

        // ── 6. JHH02 先启流 (VID/PID=1bcf:2d50)
        //    ★ 必须先开 JHH2, 再开 JHH04, 否则 JHH04 IMU 全零
        //    上锁防止与独立 JHH2 左/右双目冲突
        {
            auto& ch = ch_[1];
            fprintf(stderr, "[%s] DBG: acquiring stream_start_mutex...\n", ch.name);
            {
                std::lock_guard<std::mutex> lock(g_stream_start_mutex);
                fprintf(stderr, "[%s] DBG: LOCKED\n", ch.name);
                fprintf(stderr, "[%s] DBG: Video_DEAL_WITH_INIT...\n", ch.name);
                if (TST_USBCam_Video_DEAL_WITH_INIT(ch.tstc_handle, ch.dev_fd) != 0) {
                    fprintf(stderr, "[%s] Video_DEAL_WITH_INIT failed\n", ch.name);
                    return;
                }
                fprintf(stderr, "[%s] DBG: creating stream thread...\n", ch.name);
                pthread_create(&ch.stream_thread, nullptr, stream_thread_func, &ch);
                usleep(200000);
                fprintf(stderr, "[%s] DBG: calling STREAM_STATUS(1) blocking...\n", ch.name);
                TST_USBCam_Video_STREAM_STATUS(ch.tstc_handle, 1);
                fprintf(stderr, "[%s] DBG: STREAM_STATUS(1) done\n", ch.name);
                int rem = --g_jhh2_remaining;
                fprintf(stderr, "[%s] DBG: JHH2 done, remaining=%d\n", ch.name, rem);
            }
            fprintf(stderr, "[%s] DBG: stream_start_mutex released\n", ch.name);
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }

        // ── 7. JHH04 后启流 (VID/PID=1bcf:2d51)
        //    等待所有 JHH2 设备完成 STREAM_STATUS 后再启流
        {
            auto& ch = ch_[0];
            fprintf(stderr, "[%s] DBG: waiting for %d JHH2 devices...\n",
                    ch.name, (int)g_jhh2_remaining);
            while (g_jhh2_remaining > 0) {
                usleep(20000);
            }
            fprintf(stderr, "[%s] DBG: all JHH2 done, acquiring stream_start_mutex...\n", ch.name);
            {
                std::lock_guard<std::mutex> lock(g_stream_start_mutex);
                fprintf(stderr, "[%s] DBG: LOCKED\n", ch.name);
                fprintf(stderr, "[%s] DBG: Video_DEAL_WITH_INIT...\n", ch.name);
                if (TST_USBCam_Video_DEAL_WITH_INIT(ch.tstc_handle, ch.dev_fd) != 0) {
                    fprintf(stderr, "[%s] Video_DEAL_WITH_INIT failed\n", ch.name);
                    return;
                }
                fprintf(stderr, "[%s] DBG: creating stream thread...\n", ch.name);
                pthread_create(&ch.stream_thread, nullptr, stream_thread_func, &ch);
                usleep(200000);
                fprintf(stderr, "[%s] DBG: calling STREAM_STATUS(1) blocking...\n", ch.name);
                TST_USBCam_Video_STREAM_STATUS(ch.tstc_handle, 1);
                fprintf(stderr, "[%s] DBG: STREAM_STATUS(1) done\n", ch.name);
            }
            fprintf(stderr, "[%s] DBG: stream_start_mutex released\n", ch.name);
            ch.initialized = true;
            printf("[%s] setup OK  (%dx%d@%dfps, H265=%c, Y8=%c)\n",
                   ch.name, ch.width, ch.height, ch.fps,
                   ch.output_h265 ? 'Y' : 'N', ch.output_y8 ? 'Y' : 'N');
        }
    }

    void collect() override {
        // 为两个通道各自启动一个采集线程
        // (注意: STREAM_STATUS 已在 setup 中调用, 这里只拉帧)
        std::thread t0([this]() { collect_channel(0); });
        std::thread t1([this]() { collect_channel(1); });
        t0.join();
        t1.join();
    }

    void teardown() override {
        for (int i = 0; i < 2; i++) {
            auto& ch = ch_[i];
            if (!ch.initialized) continue;

            fprintf(stderr, "[%s] DBG teardown: ENTER\n", ch.name);

            // 停止 TSTC 流
            fprintf(stderr, "[%s] DBG teardown: stopping event loop...\n", ch.name);
            TST_USBCam_EVENT_LoopMode(ch.tstc_handle, 0);

            // 等待 stream 线程退出
            if (ch.stream_thread) {
                fprintf(stderr, "[%s] DBG teardown: joining stream thread...\n", ch.name);
                pthread_join(ch.stream_thread, nullptr);
                fprintf(stderr, "[%s] DBG teardown: stream thread joined\n", ch.name);
            }

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

            // 清理 SDK
            TST_USBCam_Video_DEAL_WITH_UNINIT(ch.tstc_handle);
            TST_USBCam_DELETE_DEVICE_POINT(ch.tstc_handle);

            printf("[%s] teardown OK\n", ch.name);
        }
    }

    // ---- TSTC SDK stream thread (每通道一个) ----
    static void* stream_thread_func(void* arg) {
        auto* ch = (SixCamChannel*)arg;
        Pix_Format fmt;
        fmt.u_PixFormat = 0;
        fmt.u_Width  = (uint32_t)ch->width;
        fmt.u_Height = (uint32_t)ch->height;
        fmt.u_Fps    = (uint32_t)ch->fps;
        TST_USBCam_Video_DEAL_WITH(ch->tstc_handle, fmt);
        return nullptr;
    }

private:
    SixCamChannel ch_[2];
    std::string session_dir_;
    int session_num_;

    v4l2_dev_sys_data_t jhh04_dev_;
    v4l2_dev_sys_data_t jhh02_dev_;

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

        uint32_t nv12_size = ch.width * ch.height * 3 / 2;
        uint8_t* nv12 = new uint8_t[nv12_size];

        fprintf(stderr, "[%s] DBG collect: entering frame loop (running=%d)\n",
                ch.name, (int)running_);

        while (running_) {
            Frame_Buffer_Data* fb = TST_USBCam_GET_FRAME_BUFF(ch.tstc_handle, 0);
            if (!fb) {
                empty_polls++;
                if (empty_polls == 1) {
                    fprintf(stderr, "[%s] DBG collect: GET_FRAME_BUFF NULL (first)\n", ch.name);
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
            uint8_t* mjpg = (uint8_t*)fb->pMem;
            size_t mjpg_len = fb->buffer.bytesused;

            // MJPEG → BGR
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000) {
                TST_USBCam_SAVE_FRAME_RES(ch.tstc_handle, fb);
                continue;
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
                    fwrite(nv12, 1, ch.width * ch.height, ch.y8_fp);
                }
            }

            delete[] bgr;
            TST_USBCam_SAVE_FRAME_RES(ch.tstc_handle, fb);
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
