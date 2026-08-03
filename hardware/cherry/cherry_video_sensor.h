#pragma once

#include "core/camera_config.h"
#include "hardware/cherry/cherry_start_control.h"
#include "hardware/common/sensor.h"
#include "hardware/video/v4l2_device.h"
#include "hardware/video/video_pipeline_stats.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <sys/types.h>

class CherryVideoSensor : public Sensor {
public:
    CherryVideoSensor(const CameraConfig& config, std::string video_path,
                      std::string session_dir, std::atomic<bool>& running,
                      CherryStartControl& start_control);

protected:
    void setup() override;
    void collect() override;
    void teardown() override;

private:
    bool open_outputs();
    bool open_fifo_writer(int timeout_ms);
    bool ffmpeg_is_alive();
    void fail_setup(const std::string& error);
    void wait_for_ffmpeg();

    CameraConfig config_;
    std::string video_path_;
    std::string session_dir_;
    std::string output_dir_;
    CherryStartControl& start_control_;
    V4l2Device device_;
    pid_t ffmpeg_pid_ = 0;
    std::string fifo_path_;
    FILE* fifo_file_ = nullptr;
    FILE* metadata_file_ = nullptr;
    VideoPipelineStats stats_;
    size_t h264_bytes_ = 0;
    std::string writer_error_;
    int ffmpeg_fallback_fd_limit_ = 0;
    bool stream_started_ = false;
    bool initialized_ = false;
};
