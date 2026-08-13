#include "app/session_runner.h"

#include "app/session_profile.h"
#include "app/capture_output_policy.h"
#include "core/barrier.h"
#include "core/output_path.h"
#include "hardware/as5600/encoder_sensor.h"
#include "hardware/cherry/cherry_serial_sensor.h"
#include "hardware/cherry/cherry_video_sensor.h"
#include "hardware/common/sensor.h"
#include "hardware/imu/imu_sensor.h"
#include "hardware/tracker/vive_tracker_sensor.h"
#include "hardware/video/sixcam_sensor.h"
#include "hardware/video/video_sensor.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

struct timespec g_t0;

namespace {

constexpr uint16_t kJhh2Vid = 0x1bcf;
constexpr uint16_t kJhh2Pid = 0x2d50;
constexpr uint16_t kSixVid = 0x1bcf;
constexpr uint16_t kSixPid = 0x2d51;

constexpr const char* kEncoderI2cPath = "/dev/i2c-6";
constexpr int kEncoderI2cAddress = 0x36;
constexpr int kEncoderIntervalUs = 10000;

}  // namespace

SessionRunner::SessionRunner(const CameraDiscoveryResult& cameras,
                             SessionOptions options,
                             std::atomic<bool>& session_running)
    : cameras_(cameras)
    , options_(options)
    , session_running_(session_running) {}

SessionRunner::~SessionRunner() = default;

std::string SessionRunner::make_session_dir(const std::string& prefix,
                                            int session_number) const {
    char path[256];
    snprintf(path, sizeof(path), "%s/session_%03d", prefix.c_str(), session_number);
    std::string session_dir(path);
    mkdir_p(path, 0755);
    for (const std::string& directory : profile_session_directories(cameras_)) {
        snprintf(path, sizeof(path), "%s/%s", session_dir.c_str(),
                 directory.c_str());
        mkdir_p(path, 0755);
    }
    return session_dir;
}

void SessionRunner::run(const std::string& session_dir,
                        int session_number,
                        const ControlPump& pump) {
    printf("\n>>> Session %d START <<<\n", session_number);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);

    time_t now = time(nullptr);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d-%02d_%02d_%02d",
             local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
             local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
    std::string session_timestamp = timestamp;

    sensors_.clear();
    cherry_start_control_.reset();

    if (camera_pipeline_for_profile(cameras_.profile) ==
        CameraPipeline::cherry_h264_remux) {
        const std::vector<CherrySensorRole> roles =
            cherry_sensor_roles(cameras_);
        if (!roles.empty()) {
            cherry_start_control_ = std::make_unique<CherryStartControl>();
            for (const CherrySensorRole role : roles) {
                if (role == CherrySensorRole::serial) {
                    sensors_.push_back(std::make_unique<CherrySerialSensor>(
                        cameras_.cherry.serial_path, "cherry_stereo", session_dir,
                        session_running_, *cherry_start_control_));
                } else {
                    sensors_.push_back(std::make_unique<CherryVideoSensor>(
                        cameras_.cherry.stereo.config,
                        cameras_.cherry.stereo.device_path, session_dir,
                        session_running_, *cherry_start_control_));
                }
            }
        }
    } else if (cameras_.profile == ProductProfile::banana) {
        const bool has_sixcam =
            cameras_.sixcam.enabled &&
            !cameras_.sixcam.jhh04_path.empty() &&
            !cameras_.sixcam.jhh02_path.empty();
        capture_control_.reset_stream_start(
            static_cast<int>(active_profile_cameras(cameras_).size()),
            has_sixcam);

        // ── 1. 六目模块 (先四目→后双目, 硬件内部已同步) ──
        if (cameras_.sixcam.enabled && !cameras_.sixcam.jhh04_path.empty() &&
            !cameras_.sixcam.jhh02_path.empty()) {
            CameraConfig jhh04{
                "jhh04", kSixVid, kSixPid, 0, 3104, 480, 30, 4000000, 30, true,
                ImuOrientation::HORIZONTAL_TOP, false, false};
            CameraConfig jhh02{
                "jhh02", kJhh2Vid, kJhh2Pid, 2, 4000, 1200, 30, 16000000, 30, true,
                ImuOrientation::HORIZONTAL_TOP, true, false};
            auto sixcam = std::make_unique<SixCamSensor>(
                jhh04, jhh02, cameras_.sixcam.jhh04_path, cameras_.sixcam.jhh02_path,
                session_dir, session_number, session_timestamp, session_running_,
                capture_control_);
            SixCamSensor* sixcam_ptr = sixcam.get();
            sensors_.push_back(std::move(sixcam));
            if (options_.use_imu) {
                sensors_.push_back(std::make_unique<ImuSensor>(
                    "jhh04", session_dir, sixcam_ptr->imu_queue_jhh04(),
                    session_number, session_timestamp,
                    ImuOrientation::HORIZONTAL_TOP, session_running_));
                sensors_.push_back(std::make_unique<ImuSensor>(
                    "jhh02", session_dir, sixcam_ptr->imu_queue_jhh02(),
                    session_number, session_timestamp,
                    ImuOrientation::HORIZONTAL_TOP, session_running_));
            }
        }

        // ── 2. 腕部相机 (六目启流完成后) ──
        for (const CameraSlot& camera : active_profile_cameras(cameras_)) {
            CameraConfig config = camera.config;
            const CameraOutputPolicy policy =
                banana_camera_output_policy(config.name);
            config.output_h265 = policy.output_h265;
            config.output_y8 = policy.output_y8;
            auto video = std::make_unique<VideoSensor>(
                config, session_dir, camera.device_path,
                session_number, session_timestamp, session_running_, capture_control_);
            VideoSensor* video_ptr = video.get();
            sensors_.push_back(std::move(video));
            if (options_.use_imu && config.has_imu) {
                sensors_.push_back(std::make_unique<ImuSensor>(
                    config.name, session_dir, video_ptr->imu_queue(), session_number,
                    session_timestamp, config.imu_orientation, session_running_));
            }
        }
    } else {
        int independent_jhh2_count = 0;
        for (const auto& camera : cameras_.jhh2) {
            if (camera.enabled && camera.config.vid == kJhh2Vid &&
                camera.config.pid == kJhh2Pid) {
                independent_jhh2_count++;
            }
        }
        const bool sixcam_jhh02_available =
            cameras_.sixcam.enabled && !cameras_.sixcam.jhh02_path.empty();
        capture_control_.reset_stream_start(independent_jhh2_count,
                                            sixcam_jhh02_available);

        for (const auto& camera : cameras_.jhh2) {
            if (!camera.enabled) {
                continue;
            }
            CameraConfig config = camera.config;
            config.output_h265 = true;
            config.output_y8 = false;
            auto video = std::make_unique<VideoSensor>(
                config, session_dir, camera.device_path,
                session_number, session_timestamp, session_running_, capture_control_);
            VideoSensor* video_ptr = video.get();
            sensors_.push_back(std::move(video));
            if (options_.use_imu && config.has_imu) {
                sensors_.push_back(std::make_unique<ImuSensor>(
                    config.name, session_dir, video_ptr->imu_queue(), session_number,
                    session_timestamp, config.imu_orientation, session_running_));
            }
        }

        if (cameras_.sixcam.enabled && !cameras_.sixcam.jhh04_path.empty() &&
            !cameras_.sixcam.jhh02_path.empty()) {
            CameraConfig jhh04{
                "jhh04", kSixVid, kSixPid, 0, 3104, 480, 30, 4000000, 30, true,
                ImuOrientation::HORIZONTAL_TOP, false, false};
            CameraConfig jhh02{
                "jhh02", kJhh2Vid, kJhh2Pid, 2, 4000, 1200, 30, 16000000, 30, true,
                ImuOrientation::HORIZONTAL_TOP, options_.use_h265, false};
            auto sixcam = std::make_unique<SixCamSensor>(
                jhh04, jhh02, cameras_.sixcam.jhh04_path, cameras_.sixcam.jhh02_path,
                session_dir, session_number, session_timestamp, session_running_,
                capture_control_);
            SixCamSensor* sixcam_ptr = sixcam.get();
            sensors_.push_back(std::move(sixcam));
            if (options_.use_imu) {
                sensors_.push_back(std::make_unique<ImuSensor>(
                    "jhh04", session_dir, sixcam_ptr->imu_queue_jhh04(),
                    session_number, session_timestamp,
                    ImuOrientation::HORIZONTAL_TOP, session_running_));
                sensors_.push_back(std::make_unique<ImuSensor>(
                    "jhh02", session_dir, sixcam_ptr->imu_queue_jhh02(),
                    session_number, session_timestamp,
                    ImuOrientation::HORIZONTAL_TOP, session_running_));
            }
        }

        if (options_.use_as5600) {
            sensors_.push_back(std::make_unique<EncoderSensor>(
                kEncoderI2cPath, kEncoderI2cAddress, session_dir, session_number,
                session_timestamp, kEncoderIntervalUs, session_running_));
        }
        if (options_.use_vive) {
            sensors_.push_back(std::make_unique<ViveTrackerSensor>(
                session_dir, session_number, session_timestamp, session_running_));
        }
    }

    if (sensors_.empty()) {
        fprintf(stderr, "WARN: no active sensors in this session\n");
        while (session_running_) {
            if (pump) {
                pump(50);
            }
        }
        return;
    }

    SimpleBarrier gate(sensors_.size());
    for (auto& sensor : sensors_) {
        sensor->launch(gate);
    }

    while (!gate.wait_all_arrived(100)) {
        if (pump) {
            pump(0);
        }
    }
    printf(">>> ALL SENSORS GO <<<\n");

    while (session_running_) {
        if (pump) {
            pump(50);
        }
    }
    printf("\n>>> Session %d STOP <<<\n", session_number);
    wait_teardown();
    printf(">>> Session %d DONE <<<\n\n", session_number);
}

void SessionRunner::wait_teardown() {
    for (auto& sensor : sensors_) {
        if (sensor) {
            sensor->join();
        }
    }
    sensors_.clear();
    cherry_start_control_.reset();
}

std::string SessionRunner::cameras_json() const {
    return profile_cameras_json(cameras_);
}

void SessionRunner::refresh_cameras(const CameraDiscoveryResult& cameras) {
    cameras_ = cameras;
}

void SessionRunner::request_preview(std::string channel, std::string path) {
    capture_control_.request_preview(std::move(channel), std::move(path));
}
