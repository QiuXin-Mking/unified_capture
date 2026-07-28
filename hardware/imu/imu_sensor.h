#pragma once
/*
 * imu_sensor.h — ImuSensor: 异步 IMU 解码 (从 FrameQueue 消费 BGR 帧)
 */

#include "hardware/common/sensor.h"
#include "hardware/imu/imu_decode.h"
#include "core/frame_queue.h"
#include "core/camera_config.h"

class ImuSensor : public Sensor {
public:
    ImuSensor(const std::string& camera_name,
              const std::string& session_dir,
              FrameQueue& queue,
              int session_num,
              const std::string& session_ts,
              ImuOrientation orientation,
              std::atomic<bool>& running)
        : Sensor(camera_name + "_imu", running)
        , camera_name_(camera_name)
        , session_dir_(session_dir)
        , queue_(queue)
        , session_num_(session_num)
        , session_ts_(session_ts)
        , orientation_(orientation) {}

protected:
    void setup() override {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s/%s-%s.jsonl",
                 session_dir_.c_str(), camera_name_.c_str(), camera_name_.c_str(), session_ts_.c_str());
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
    std::string session_ts_;
    ImuOrientation orientation_;
    FILE* fp_ = nullptr;
};
