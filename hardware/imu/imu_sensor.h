#pragma once
/*
 * imu_sensor.h — ImuSensor: 异步 IMU 解码 (从 FrameQueue 消费 BGR 帧)
 */

#include "hardware/common/sensor.h"
#include "hardware/imu/imu_decode.h"
#include "hardware/imu/imu_frame_queue.h"
// @deprecated: core/frame_queue.h 仅用于以下 legacy 构造函数和
// legacy_queue_ 代码路径，session_runner.cpp 已全部使用 ImuFrameQueue 构造。
// 后续可一并删除此 include、legacy 构造函数和 collect() 中 legacy_queue_ 分支。
#include "core/frame_queue.h"
#include "core/camera_config.h"

#include <unistd.h>

class ImuSensor : public Sensor {
public:
    // @deprecated: legacy 构造函数，使用旧的 FrameQueue (BGRFrame)。
    // session_runner.cpp 已全部使用下方 ImuFrameQueue 构造，此构造函数无调用方。
    // 后续可连同 FrameQueue/BGRFrame/legacy_queue_/collect() 旧分支一并删除。
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
        , legacy_queue_(&queue)
        , session_num_(session_num)
        , session_ts_(session_ts)
        , orientation_(orientation) {}

    ImuSensor(const std::string& camera_name,
              const std::string& session_dir,
              ImuFrameQueue& queue,
              int session_num,
              const std::string& session_ts,
              ImuOrientation orientation,
              std::atomic<bool>& running)
        : Sensor(camera_name + "_imu", running)
        , camera_name_(camera_name)
        , session_dir_(session_dir)
        , compact_queue_(&queue)
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
        uint64_t total_frames = 0;
        uint64_t imu_frames = 0;
        uint64_t total_bytes = 0;
        if (compact_queue_) {
            ImuFrame frame;
            while (compact_queue_->wait_pop(frame)) {
                total_frames++;
                if (fp_ && frame.size >= IMU_GROUP) {
                    imu_parse_and_write(
                        frame.data.data(), frame.size, frame.frame_idx, fp_);
                    imu_frames++;
                    total_bytes += frame.size;
                }
            }
        }
        // @deprecated: legacy IMU 解码路径 (从 BGR 帧扫描码带)。
        // session_runner.cpp 已全部使用 ImuFrameQueue，
        // IMU 解码现在在 VideoFrameProcessor 中完成 (imu_read_luma_* )。
        // 此分支永远不执行 (legacy_queue_ 始终为 nullptr)，后续可删除。
        while (legacy_queue_ && (running_ || !legacy_queue_->empty())) {
            BGRFrame frame;
            if (legacy_queue_->try_pop(frame)) {
                total_frames++;
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
                        imu_frames++;
                        total_bytes += imu_len;
                    }
                }
            } else {
                usleep(5000);
            }
        }
        fprintf(stderr, "[%s] IMU: %llu frames scanned, %llu with data, %llu bytes total\n",
                camera_name_.c_str(), (unsigned long long)total_frames,
                (unsigned long long)imu_frames, (unsigned long long)total_bytes);
    }

    void teardown() override {
        if (fp_) { fclose(fp_); fp_ = nullptr; }
    }

private:
    std::string camera_name_;
    std::string session_dir_;
    // @deprecated: legacy_queue_ 仅在不使用的 legacy 构造中赋值，
    // 生产路径始终为 nullptr。后续可连同 FrameQueue/BGRFrame 一并删除。
    FrameQueue* legacy_queue_ = nullptr;
    ImuFrameQueue* compact_queue_ = nullptr;
    int session_num_;
    std::string session_ts_;
    ImuOrientation orientation_;
    FILE* fp_ = nullptr;
};
