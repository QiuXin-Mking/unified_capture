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
#include "imu_decode.h"

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
// BGR24 → NV12 (YUV420SP) 色域转换
// ============================================================
static void bgr_to_nv12(const uint8_t* bgr, int w, int h, uint8_t* nv12) {
    int ys = w * h;
    uint8_t* Y  = nv12;
    uint8_t* UV = nv12 + ys;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int off = (r * w + c) * 3;
            int R = bgr[off + 2], G = bgr[off + 1], B = bgr[off];
            Y[r * w + c] = (uint8_t)((66 * R + 129 * G + 25 * B + 128) >> 8);
            if ((r & 1) == 0 && (c & 1) == 0) {
                int uv_idx = (r / 2) * w + (c & ~1);
                UV[uv_idx]     = (uint8_t)((-38 * R - 74 * G + 112 * B + 128 + 32768) >> 8);
                UV[uv_idx + 1] = (uint8_t)((112 * R - 94 * G - 18 * B + 128 + 32768) >> 8);
            }
        }
    }
}

// ============================================================
// MPP H.265 硬件编码器
// ============================================================
struct MppEncoder {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup buf_group = nullptr;
    uint32_t frame_size = 0;
    uint32_t width = 0, height = 0;

    bool init(uint32_t w, uint32_t h, int bps, int fps, int gop) {
        width = w; height = h;
        frame_size = w * h * 3 / 2;
        MPP_RET ret;

        ret = mpp_create(&ctx, &mpi);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_create failed\n"); return false; }

        ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_init failed\n"); return false; }

        // Prep 配置
        MppEncPrepCfg prep;
        memset(&prep, 0, sizeof(prep));
        prep.change      = 0xFFFFFFFF;
        prep.width       = w;
        prep.height      = h;
        prep.hor_stride  = w;
        prep.ver_stride  = h;
        prep.format      = MPP_FMT_YUV420SP;
        ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, &prep);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_PREP_CFG failed\n"); return false; }

        // RC 配置 (CBR)
        MppEncRcCfg rc;
        memset(&rc, 0, sizeof(rc));
        rc.change       = 0xFFFFFFFF;
        rc.rc_mode      = MPP_ENC_RC_MODE_CBR;
        rc.bps_target   = bps;
        rc.bps_max      = bps * 3 / 2;
        rc.bps_min      = bps / 8;
        rc.fps_in_num   = fps;
        rc.fps_in_denorm  = 1;
        rc.fps_out_num  = fps;
        rc.fps_out_denorm = 1;
        rc.gop          = gop;
        ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, &rc);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_RC_CFG failed\n"); return false; }

        // Codec 配置
        MppEncCodecCfg codec;
        memset(&codec, 0, sizeof(codec));
        codec.coding = MPP_VIDEO_CodingHEVC;
        codec.h265.change = 0xFFFFFFFF;
        ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, &codec);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_CODEC_CFG failed\n"); return false; }

        // Buffer group
        ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM,
                                   MPP_BUFFER_INTERNAL, "he", NULL);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_buffer_group_get failed\n"); return false; }

        return true;
    }

    // 编码一帧 NV12 → H.265 NAL, 直接写 FILE*
    size_t put(uint8_t* nv12, FILE* fp) {
        MppFrame frame;
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, width);
        mpp_frame_set_ver_stride(frame, height);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(frame, 0);

        MppBuffer buf = nullptr;
        mpp_buffer_get(buf_group, &buf, frame_size);
        memcpy(mpp_buffer_get_ptr(buf), nv12, frame_size);
        mpp_frame_set_buffer(frame, buf);

        mpi->encode_put_frame(ctx, frame);
        mpp_frame_deinit(&frame);

        // 取编码结果
        MppPacket pkt = nullptr;
        size_t written = 0;
        if (!mpi->encode_get_packet(ctx, &pkt) && pkt && mpp_packet_get_length(pkt) > 0) {
            written = mpp_packet_get_length(pkt);
            fwrite(mpp_packet_get_data(pkt), 1, written, fp);
        }
        if (pkt) mpp_packet_deinit(&pkt);
        return written;
    }

    // Flush 编码器 (发送 EOS, 排空残余帧)
    size_t flush(FILE* fp) {
        size_t total = 0;
        MppFrame eos;
        mpp_frame_init(&eos);
        mpp_frame_set_eos(eos, 1);
        mpi->encode_put_frame(ctx, eos);
        mpp_frame_deinit(&eos);

        for (int i = 0; i < 20; i++) {
            MppPacket pkt = nullptr;
            if (mpi->encode_get_packet(ctx, &pkt) || !pkt) break;
            size_t len = mpp_packet_get_length(pkt);
            if (len) { fwrite(mpp_packet_get_data(pkt), 1, len, fp); total += len; }
            mpp_packet_deinit(&pkt);
        }
        return total;
    }

    void destroy() {
        if (buf_group) { mpp_buffer_group_put(buf_group); buf_group = nullptr; }
        if (ctx) {
            mpi->reset(ctx);
            mpp_destroy(ctx);
            ctx = nullptr;
        }
    }
};

#include "camera_config.h"

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

        // ── 创建输出目录 ──
        snprintf(path, sizeof(path), "%s/%s", session_dir_.c_str(), cfg_.name);
        out_dir_ = path;
        mkdir_p(out_dir_.c_str(), 0755);

        // ── 1. TSTC SDK 初始化 ──
        tstc_handle_ = TST_USBCam_CREATE_DEVICE_POINT(dev_info_);
        if (!tstc_handle_) {
            fprintf(stderr, "[%s] TST_USBCam_CREATE_DEVICE_POINT failed\n", cfg_.name);
            return;
        }

        dev_fd_ = open(dev_info_.Device_Path, O_RDWR | O_NONBLOCK);
        if (dev_fd_ < 0) {
            fprintf(stderr, "[%s] open %s failed\n", cfg_.name, dev_info_.Device_Path);
            return;
        }

        if (TST_USBCam_Video_DEAL_WITH_INIT(tstc_handle_, dev_fd_) != 0) {
            fprintf(stderr, "[%s] Video_DEAL_WITH_INIT failed\n", cfg_.name);
            return;
        }

        // ── 2. MPP 编码器 ──
        if (cfg_.output_h265) {
            if (!mpp_.init(cfg_.width, cfg_.height, cfg_.bitrate, cfg_.fps, cfg_.gop)) {
                fprintf(stderr, "[%s] MPP init failed\n", cfg_.name);
                return;
            }
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

            // 打开 FIFO 写入端 (阻塞直到 ffmpeg 打开读端)
            fifo_fd_ = open(fifo_path_.c_str(), O_WRONLY);
            if (fifo_fd_ < 0) {
                perror("open fifo for write");
                return;
            }
            fifo_fp_ = fdopen(fifo_fd_, "w");
        }

        // ── 4. Y8 原始灰度文件 ──
        if (cfg_.output_y8) {
            snprintf(path, sizeof(path), "%s/%03d.y8",
                     out_dir_.c_str(), session_num_);
            y8_fp_ = fopen(path, "w");
            if (!y8_fp_) {
                fprintf(stderr, "[%s] cannot create Y8 file %s\n", cfg_.name, path);
            }
        }

        // ── 5. 启动 TSTC 流线程 ──
        pthread_create(&stream_thread_, nullptr, VideoSensor::stream_thread_func, this);

        printf("[%s] setup OK  (dev=%s, %dx%d@%dfps, H265=%c, Y8=%c)\n",
               cfg_.name, dev_info_.Device_Path, cfg_.width, cfg_.height, cfg_.fps,
               cfg_.output_h265 ? 'Y' : 'N', cfg_.output_y8 ? 'Y' : 'N');
        initialized_ = true;
    }

    void collect() override {
        if (!initialized_) {
            fprintf(stderr, "[%s] not initialized, skip\n", cfg_.name);
            return;
        }

        // ★ 启动视频流 (所有线程在 barrier 之后同时到达这里)
        TST_USBCam_Video_STREAM_STATUS(tstc_handle_, 1);

        tjhandle tj = tjInitDecompress();
        uint64_t frame_idx = 0;
        size_t total_h265 = 0;

        uint32_t nv12_size = cfg_.width * cfg_.height * 3 / 2;
        uint8_t* nv12 = new uint8_t[nv12_size];

        while (running_) {
            Frame_Buffer_Data* fb = TST_USBCam_GET_FRAME_BUFF(tstc_handle_, 0);
            if (!fb) {
                usleep(1000);
                continue;
            }

            uint64_t ts_us = elapsed_us();
            uint8_t* mjpg = (uint8_t*)fb->pMem;
            size_t mjpg_len = fb->buffer.bytesused;

            // --- MJPEG → BGR (turbojpeg) ---
            int w = 0, h = 0, subsamp = 0;
            if (tjDecompressHeader2(tj, mjpg, mjpg_len, &w, &h, &subsamp) != 0 ||
                w <= 0 || w > 8000) {
                TST_USBCam_SAVE_FRAME_RES(tstc_handle_, fb);
                continue;
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
                    fwrite(nv12, 1, cfg_.width * cfg_.height, y8_fp_);
                }
            }

            delete[] bgr;
            TST_USBCam_SAVE_FRAME_RES(tstc_handle_, fb);
            frame_idx++;
        }

        // 停止流
        TST_USBCam_EVENT_LoopMode(tstc_handle_, 0);

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
    }

    void teardown() override {
        if (cfg_.output_h265) {
            if (fifo_fp_) { fclose(fifo_fp_); fifo_fp_ = nullptr; fifo_fd_ = -1; }

            // 等待 ffmpeg 退出 (FIFO 读端收到 EOF)
            if (ffmpeg_pid_ > 0) {
                int status;
                waitpid(ffmpeg_pid_, &status, 0);
                printf("[%s] ffmpeg exited (%d)\n", cfg_.name, WEXITSTATUS(status));
            }
        }

        // 等待 TSTC 流线程
        if (stream_thread_) {
            pthread_join(stream_thread_, nullptr);
        }

        // 清理 MPP
        if (cfg_.output_h265) { mpp_.destroy(); }

        // 清理 FIFO
        if (cfg_.output_h265) { unlink(fifo_path_.c_str()); }

        // 关闭 Y8 文件
        if (y8_fp_) { fclose(y8_fp_); y8_fp_ = nullptr; }

        printf("[%s] teardown OK\n", cfg_.name);
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

// ============================================================
// ImuSensor — 异步 IMU 解码 (从 FrameQueue 消费 BGR 帧)
// ============================================================
class ImuSensor : public Sensor {
public:
    ImuSensor(const std::string& camera_name,
              const std::string& session_dir,
              FrameQueue& queue,
              int session_num,
              ImuOrientation orientation,
              std::atomic<bool>& running)
        : Sensor(camera_name + "_imu", running)
        , camera_name_(camera_name)
        , session_dir_(session_dir)
        , queue_(queue)
        , session_num_(session_num)
        , orientation_(orientation) {}

protected:
    void setup() override {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s/%03d_imu.jsonl",
                 session_dir_.c_str(), camera_name_.c_str(), session_num_);
        fp_ = fopen(path, "w");
    }

    void collect() override {
        while (running_ || !queue_.empty()) {
            BGRFrame frame;
            if (queue_.try_pop(frame)) {
                if (fp_ && !frame.data.empty()) {
                    uint8_t imu_buf[256] = {};
                    uint32_t imu_len = 0;

                    // 根据码带方向选择扫描策略
                    if (orientation_ == ImuOrientation::HORIZONTAL_TOP) {
                        imu_len = imu_read_frame_horizontal(
                            frame.data.data(), frame.width, frame.height, imu_buf);
                    } else {
                        imu_len = imu_read_frame_vertical(
                            frame.data.data(), frame.width, frame.height, imu_buf);
                    }

                    if (imu_len >= IMU_GROUP) {
                        imu_parse_and_write(imu_buf, imu_len, frame.frame_idx, fp_);
                    }
                }
            } else {
                usleep(5000);
            }
        }
    }

    void teardown() override {
        if (fp_) { fclose(fp_); fp_ = nullptr; }
    }

private:
    std::string camera_name_;
    std::string session_dir_;
    FrameQueue& queue_;
    int session_num_;
    ImuOrientation orientation_;
    FILE* fp_ = nullptr;
};
