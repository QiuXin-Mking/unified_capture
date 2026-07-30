#pragma once

#include "core/camera_config.h"
#include "hardware/common/sensor.h"
#include "hardware/imu/imu_frame_queue.h"
#include "hardware/video/capture_control.h"
#include "hardware/video/capture_pipeline.h"
#include "hardware/video/mpp_encoder.h"
#include "hardware/video/v4l2_device.h"
#include "hardware/video/video_frame_processor.h"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

struct SixCamChannel {
    const char* name = nullptr;
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;
    int gop = 0;
    bool output_h265 = false;
    bool output_y8 = false;
    bool has_imu = false;
    ImuOrientation imu_orientation = ImuOrientation::HORIZONTAL_TOP;
    std::string device_path;
    V4l2Device device;
    MppEncoder mpp;
    pid_t ffmpeg_pid = 0;
    std::string fifo_path;
    int fifo_fd = -1;
    FILE* fifo_fp = nullptr;
    FILE* y8_fp = nullptr;
    ImuFrameQueue imu_queue{64};
    VideoPipelineStats stats;
    int actual_width = 0;
    int actual_height = 0;
    int nv12_stride = 0;
    std::string out_dir;
    bool stream_started = false;
    bool initialized = false;
};

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
        , control_(control) {
        configure_channel(ch_[0], jhh04_cfg, jhh04_path);
        configure_channel(ch_[1], jhh02_cfg, jhh02_path);
    }

    ImuFrameQueue& imu_queue_jhh04() { return ch_[0].imu_queue; }
    ImuFrameQueue& imu_queue_jhh02() { return ch_[1].imu_queue; }

protected:
    void setup() override {
        char path[512];
        for (auto& ch : ch_) {
            snprintf(path, sizeof(path), "%s/%s",
                     session_dir_.c_str(), ch.name);
            ch.out_dir = path;
            mkdir_p(path, 0755);
            if (!ch.device.open(
                    ch.device_path, ch.width, ch.height, ch.fps)) {
                fprintf(stderr, "[%s] V4L2 open failed\n", ch.name);
                return;
            }
            ch.actual_width = ch.device.actual_width();
            ch.actual_height = ch.device.actual_height();
            ch.nv12_stride = (ch.actual_width + 63) & ~63;
        }

        for (auto& ch : ch_) {
            if (ch.output_h265 &&
                !ch.mpp.init(ch.actual_width, ch.actual_height,
                             ch.bitrate, ch.fps, ch.gop)) {
                fprintf(stderr, "[%s] MPP init failed\n", ch.name);
                return;
            }
            if (ch.output_h265 &&
                !open_h265_output(ch, path, sizeof(path))) {
                return;
            }
            if (ch.output_y8) {
                snprintf(path, sizeof(path), "%s/%s-%s.y8",
                         ch.out_dir.c_str(), ch.name, session_ts_.c_str());
                ch.y8_fp = fopen(path, "wb");
                if (!ch.y8_fp) {
                    fprintf(stderr, "[%s] cannot create Y8 output\n", ch.name);
                    return;
                }
            }
        }

        SixCamChannel& jhh02 = ch_[1];
        if (!jhh02.device.start_stream()) {
            fprintf(stderr, "[%s] start_stream failed\n", jhh02.name);
            return;
        }
        jhh02.stream_started = true;
        jhh02.initialized = true;
        control_.mark_jhh02_started();
        printf("[%s] setup OK (%dx%d@%dfps, H265=%c, Y8=%c)\n",
               jhh02.name, jhh02.actual_width, jhh02.actual_height,
               jhh02.fps, jhh02.output_h265 ? 'Y' : 'N',
               jhh02.output_y8 ? 'Y' : 'N');

        for (int i = 0; i < 500 && !control_.jhh04_may_start(); ++i) {
            usleep(20000);
        }
        if (!control_.jhh04_may_start()) {
            fprintf(stderr, "[jhh04] timed out waiting for wrist streams\n");
            return;
        }

        SixCamChannel& jhh04 = ch_[0];
        if (!jhh04.device.start_stream()) {
            fprintf(stderr, "[%s] start_stream failed\n", jhh04.name);
            return;
        }
        jhh04.stream_started = true;
        jhh04.initialized = true;
        printf("[%s] setup OK (%dx%d@%dfps, H265=%c, Y8=%c)\n",
               jhh04.name, jhh04.actual_width, jhh04.actual_height,
               jhh04.fps, jhh04.output_h265 ? 'Y' : 'N',
               jhh04.output_y8 ? 'Y' : 'N');
    }

    void collect() override {
        std::thread jhh04([this] { collect_channel(ch_[0]); });
        std::thread jhh02([this] { collect_channel(ch_[1]); });
        jhh04.join();
        jhh02.join();
    }

    void teardown() override {
        for (auto& ch : ch_) {
            if (ch.stream_started) {
                ch.device.stop_stream();
                ch.stream_started = false;
            }
            if (ch.fifo_fp) {
                fclose(ch.fifo_fp);
                ch.fifo_fp = nullptr;
                ch.fifo_fd = -1;
            }
            if (ch.ffmpeg_pid > 0) {
                int status = 0;
                waitpid(ch.ffmpeg_pid, &status, 0);
                printf("[%s] ffmpeg exited (%d)\n", ch.name,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                ch.ffmpeg_pid = 0;
            }
            if (ch.y8_fp) {
                fclose(ch.y8_fp);
                ch.y8_fp = nullptr;
            }
            ch.device.close();
            if (ch.output_h265) {
                ch.mpp.destroy();
                unlink(ch.fifo_path.c_str());
            }
            printf("[%s] teardown OK\n", ch.name);
        }
    }

private:
    static void configure_channel(SixCamChannel& channel,
                                  const CameraConfig& config,
                                  const std::string& device_path) {
        channel.name = config.name;
        channel.width = config.width;
        channel.height = config.height;
        channel.fps = config.fps;
        channel.bitrate = config.bitrate;
        channel.gop = config.gop;
        channel.output_h265 = config.output_h265;
        channel.output_y8 = config.output_y8;
        channel.has_imu = config.has_imu;
        channel.imu_orientation = config.imu_orientation;
        channel.device_path = device_path;
    }

    bool open_h265_output(SixCamChannel& ch,
                          char* path, size_t path_size) {
        snprintf(path, path_size, "/tmp/h265_%s_fifo", ch.name);
        ch.fifo_path = path;
        unlink(ch.fifo_path.c_str());
        if (mkfifo(ch.fifo_path.c_str(), 0666) < 0) {
            perror("mkfifo");
            return false;
        }

        ch.ffmpeg_pid = fork();
        if (ch.ffmpeg_pid < 0) {
            perror("fork ffmpeg");
            return false;
        }
        if (ch.ffmpeg_pid == 0) {
            snprintf(path, path_size, "%s/%s-%s.mkv",
                     ch.out_dir.c_str(), ch.name, session_ts_.c_str());
            char fps[16];
            snprintf(fps, sizeof(fps), "%d", ch.fps);
            execlp("ffmpeg", "ffmpeg",
                   "-y", "-hide_banner", "-loglevel", "error",
                   "-f", "hevc", "-r", fps,
                   "-i", ch.fifo_path.c_str(),
                   "-c", "copy", path, nullptr);
            perror("exec ffmpeg");
            _exit(1);
        }

        ch.fifo_fd = open(ch.fifo_path.c_str(), O_WRONLY);
        if (ch.fifo_fd < 0) {
            perror("open fifo for write");
            return false;
        }
        ch.fifo_fp = fdopen(ch.fifo_fd, "wb");
        return ch.fifo_fp != nullptr;
    }

    void collect_channel(SixCamChannel& ch) {
        if (!ch.initialized) {
            ch.imu_queue.close();
            return;
        }

        VideoFrameProcessorOptions options{
            ch.name,
            ch.output_h265,
            ch.output_y8,
            ch.has_imu,
            ch.imu_orientation,
            ch.actual_width,
            ch.actual_height,
            ch.nv12_stride,
        };
        VideoFrameProcessor processor(
            options, ch.output_h265 ? &ch.mpp : nullptr,
            ch.fifo_fp, ch.y8_fp, ch.has_imu ? &ch.imu_queue : nullptr,
            ch.output_h265 ? &control_ : nullptr);

        const auto start = std::chrono::steady_clock::now();
        ch.stats = run_capture_pipeline(ch.device, running_, processor, 12);
        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
        if (ch.stream_started) {
            ch.device.stop_stream();
            ch.stream_started = false;
        }
        if (ch.output_h265) {
            ch.mpp.flush(ch.fifo_fp);
        }

        const double seconds = elapsed_us > 0 ? elapsed_us / 1000000.0 : 0.0;
        const double acquired_fps =
            seconds > 0 ? ch.stats.acquired / seconds : 0.0;
        const double processed_fps =
            seconds > 0 ? ch.stats.processed / seconds : 0.0;
        const auto& timing = processor.timings();
        const double frames =
            timing.frames > 0 ? static_cast<double>(timing.frames) : 1.0;
        fprintf(stderr,
                "[%s] PIPELINE acquired=%llu processed=%llu "
                "acquired_fps=%.2f processed_fps=%.2f gaps=%llu "
                "overflows=%llu decode_failures=%llu encoder_failures=%llu "
                "imu_overflows=%llu decode_us=%.1f imu_us=%.1f "
                "nv12_us=%.1f encoder_submit_us=%.1f encoder_us=%.1f "
                "h265_bytes=%zu\n",
                ch.name,
                static_cast<unsigned long long>(ch.stats.acquired),
                static_cast<unsigned long long>(ch.stats.processed),
                acquired_fps, processed_fps,
                static_cast<unsigned long long>(ch.stats.sequence_gaps),
                static_cast<unsigned long long>(ch.stats.queue_overflows),
                static_cast<unsigned long long>(ch.stats.decode_failures),
                static_cast<unsigned long long>(ch.stats.encoder_failures),
                static_cast<unsigned long long>(ch.stats.imu_queue_overflows),
                timing.decode_us / frames, timing.imu_us / frames,
                timing.nv12_us / frames,
                timing.encoder_submit_us / frames,
                timing.encoder_us / frames,
                processor.total_h265_bytes());
    }

    SixCamChannel ch_[2];
    std::string session_dir_;
    int session_num_;
    std::string session_ts_;
    VideoCaptureControl& control_;
};
