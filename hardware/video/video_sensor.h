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
#include <unistd.h>

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

    ImuFrameQueue& imu_queue() { return imu_queue_; }

protected:
    void setup() override {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s",
                 session_dir_.c_str(), cfg_.name);
        out_dir_ = path;
        mkdir_p(out_dir_.c_str(), 0755);

        if (!device_.open(
                device_path_, cfg_.width, cfg_.height, cfg_.fps)) {
            fprintf(stderr, "[%s] V4L2 open failed\n", cfg_.name);
            return;
        }

        actual_width_ = device_.actual_width();
        actual_height_ = device_.actual_height();
        nv12_stride_ = (actual_width_ + 63) & ~63;

        if (cfg_.output_h265 &&
            !mpp_.init(actual_width_, actual_height_,
                       cfg_.bitrate, cfg_.fps, cfg_.gop)) {
            fprintf(stderr, "[%s] MPP init failed\n", cfg_.name);
            return;
        }

        if (cfg_.output_h265 && !open_h265_output(path, sizeof(path))) {
            return;
        }
        if (cfg_.output_y8) {
            snprintf(path, sizeof(path), "%s/%s-%s.y8",
                     out_dir_.c_str(), cfg_.name, session_ts_.c_str());
            y8_fp_ = fopen(path, "wb");
            if (!y8_fp_) {
                fprintf(stderr, "[%s] cannot create Y8 file %s\n",
                        cfg_.name, path);
                return;
            }
        }

        for (int i = 0; i < 500 && !control_.wrists_may_start(); ++i) {
            usleep(20000);
        }
        if (!control_.wrists_may_start()) {
            fprintf(stderr, "[%s] timed out waiting for JHH02\n", cfg_.name);
            return;
        }
        if (!device_.start_stream()) {
            fprintf(stderr, "[%s] start_stream failed\n", cfg_.name);
            return;
        }
        stream_started_ = true;
        control_.mark_wrist_started();
        initialized_ = true;
        printf("[%s] setup OK (%dx%d@%dfps, H265=%c, Y8=%c)\n",
               cfg_.name, actual_width_, actual_height_, cfg_.fps,
               cfg_.output_h265 ? 'Y' : 'N',
               cfg_.output_y8 ? 'Y' : 'N');
    }

    void collect() override {
        if (!initialized_) {
            imu_queue_.close();
            return;
        }

        VideoFrameProcessorOptions options{
            cfg_.name,
            cfg_.output_h265,
            cfg_.output_y8,
            cfg_.has_imu,
            cfg_.imu_orientation,
            actual_width_,
            actual_height_,
            nv12_stride_,
        };
        VideoFrameProcessor processor(
            options, cfg_.output_h265 ? &mpp_ : nullptr,
            fifo_fp_, y8_fp_, cfg_.has_imu ? &imu_queue_ : nullptr,
            &control_);

        const auto start = std::chrono::steady_clock::now();
        stats_ = run_capture_pipeline(device_, running_, processor, 12);
        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start)
                .count();

        if (stream_started_) {
            device_.stop_stream();
            stream_started_ = false;
        }
        if (cfg_.output_h265) {
            mpp_.flush(fifo_fp_);
        }

        const double seconds = elapsed_us > 0 ? elapsed_us / 1000000.0 : 0.0;
        const double acquired_fps =
            seconds > 0 ? stats_.acquired / seconds : 0.0;
        const double processed_fps =
            seconds > 0 ? stats_.processed / seconds : 0.0;
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
                cfg_.name,
                static_cast<unsigned long long>(stats_.acquired),
                static_cast<unsigned long long>(stats_.processed),
                acquired_fps, processed_fps,
                static_cast<unsigned long long>(stats_.sequence_gaps),
                static_cast<unsigned long long>(stats_.queue_overflows),
                static_cast<unsigned long long>(stats_.decode_failures),
                static_cast<unsigned long long>(stats_.encoder_failures),
                static_cast<unsigned long long>(stats_.imu_queue_overflows),
                timing.decode_us / frames, timing.imu_us / frames,
                timing.nv12_us / frames,
                timing.encoder_submit_us / frames,
                timing.encoder_us / frames,
                processor.total_h265_bytes());
    }

    void teardown() override {
        if (stream_started_) {
            device_.stop_stream();
            stream_started_ = false;
        }
        if (fifo_fp_) {
            fclose(fifo_fp_);
            fifo_fp_ = nullptr;
            fifo_fd_ = -1;
        }
        if (ffmpeg_pid_ > 0) {
            int status = 0;
            waitpid(ffmpeg_pid_, &status, 0);
            printf("[%s] ffmpeg exited (%d)\n", cfg_.name,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            ffmpeg_pid_ = 0;
        }
        if (y8_fp_) {
            fclose(y8_fp_);
            y8_fp_ = nullptr;
        }
        device_.close();
        if (cfg_.output_h265) {
            mpp_.destroy();
            unlink(fifo_path_.c_str());
        }
        printf("[%s] teardown OK\n", cfg_.name);
    }

private:
    bool open_h265_output(char* path, size_t path_size) {
        snprintf(path, path_size, "/tmp/h265_%s_fifo", cfg_.name);
        fifo_path_ = path;
        unlink(fifo_path_.c_str());
        if (mkfifo(fifo_path_.c_str(), 0666) < 0) {
            perror("mkfifo");
            return false;
        }

        ffmpeg_pid_ = fork();
        if (ffmpeg_pid_ < 0) {
            perror("fork ffmpeg");
            return false;
        }
        if (ffmpeg_pid_ == 0) {
            snprintf(path, path_size, "%s/%s-%s.mkv",
                     out_dir_.c_str(), cfg_.name, session_ts_.c_str());
            char fps[16];
            snprintf(fps, sizeof(fps), "%d", cfg_.fps);
            execlp("ffmpeg", "ffmpeg",
                   "-y", "-hide_banner", "-loglevel", "error",
                   "-f", "hevc", "-r", fps,
                   "-i", fifo_path_.c_str(),
                   "-c", "copy", path, nullptr);
            perror("exec ffmpeg");
            _exit(1);
        }

        fifo_fd_ = open(fifo_path_.c_str(), O_WRONLY);
        if (fifo_fd_ < 0) {
            perror("open fifo for write");
            return false;
        }
        fifo_fp_ = fdopen(fifo_fd_, "wb");
        return fifo_fp_ != nullptr;
    }

    CameraConfig cfg_;
    std::string session_dir_;
    std::string out_dir_;
    std::string device_path_;
    int session_num_;
    std::string session_ts_;
    VideoCaptureControl& control_;
    V4l2Device device_;
    MppEncoder mpp_;
    pid_t ffmpeg_pid_ = 0;
    std::string fifo_path_;
    int fifo_fd_ = -1;
    FILE* fifo_fp_ = nullptr;
    FILE* y8_fp_ = nullptr;
    ImuFrameQueue imu_queue_{64};
    VideoPipelineStats stats_;
    int actual_width_ = 0;
    int actual_height_ = 0;
    int nv12_stride_ = 0;
    bool stream_started_ = false;
    bool initialized_ = false;
};
