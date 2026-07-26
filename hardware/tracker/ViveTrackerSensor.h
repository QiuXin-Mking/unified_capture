#pragma once
/*
 * ViveTrackerSensor.h — VIVE Tracker 3.0 位姿采集 + 离线重采样
 *
 * 采集: libsurvive Low-Level API, 全速写入 tracker_raw.jsonl
 * 重采样: 最近邻/线性插值, 固定 Hz 输出, 多设备时间对齐
 *
 * 目录: hardware/tracker/
 * 依赖: sensor.h, vive_usb.h, libsurvive
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

#include "../../sensor.h"
#include "../../vive_usb.h"
#include "libsurvive/survive.h"

// ============================================================
// 位姿记录 (缓冲用)
// ============================================================
struct PoseRecord {
    uint64_t timecode;   // libsurvive 48MHz 时间戳
    char     codename[8];
    float    x, y, z;
    float    qw, qx, qy, qz;
};

// ============================================================
// 设备位姿序列 (用于重采样)
// ============================================================
struct DevicePoseSeries {
    std::string name;
    std::vector<PoseRecord> poses;  // 按 timecode 升序
    uint64_t t_min = UINT64_MAX;
    uint64_t t_max = 0;
};

// ============================================================
// 重采样配置
// ============================================================
struct ResampleConfig {
    double target_hz = 100.0;           // 目标频率
    bool   enable   = true;             // 是否启用重采样
    bool   use_interp = false;          // true=线性插值, false=最近邻
    uint64_t tc_interval = 480000;      // timecode 间隔 (48MHz/100Hz)
};

// ============================================================
// ViveTrackerSensor
// ============================================================
class ViveTrackerSensor : public Sensor {
public:
    ViveTrackerSensor(const std::string& session_dir,
                      int session_num,
                      std::atomic<bool>& running,
                      const ResampleConfig& cfg = ResampleConfig{});

    ~ViveTrackerSensor() override;

protected:
    void setup() override;
    void collect() override;
    void teardown() override;

private:
    // ---- 采集 ----
    std::string       session_dir_;
    int               session_num_;
    SurviveContext*   ctx_ = nullptr;
    FILE*             fp_raw_ = nullptr;       // tracker_raw.jsonl
    FILE*             fp_angle_ = nullptr;     // tracker_angle.jsonl
    std::string       dev_name_;               // sysfs 设备名, 用于回绑
    uint64_t          pose_count_ = 0;
    uint64_t          angle_count_ = 0;
    uint64_t          light_count_ = 0;
    bool              initialized_ = false;
    bool              ctx_error_ = false;

    // ---- 重采样缓冲 ----
    ResampleConfig                resample_cfg_;
    std::vector<PoseRecord>       raw_buffer_;       // 全速原始位姿
    static constexpr size_t       kBufferReserve = 50000;  // 预分配

    // ---- libsurvive 回调 ----
    static inline ViveTrackerSensor* s_instance_ = nullptr;

    static void pose_callback(SurviveObject* so, uint64_t timecode,
                              const SurvivePose* pose);
    static void angle_callback(SurviveObject* so, int sensor_id, int acode,
                               survive_timecode timecode, FLT length,
                               FLT angle, uint32_t lh);
    static void light_callback(SurviveObject* so, int sensor_id, int acode,
                               int timeinsweep, survive_timecode timecode,
                               survive_timecode length, uint32_t lighthouse);

    // ---- 重采样 ----
    void resample_and_write();

    // 最近邻: 在有序序列中找 timecode 最接近的 pose
    static const PoseRecord* nearest(const std::vector<PoseRecord>& poses,
                                     uint64_t target_tc);

    // 线性插值: 在前后两个 pose 之间插值
    static bool interpolate(const std::vector<PoseRecord>& poses,
                            uint64_t target_tc,
                            float out[7]);

    // lerp 辅助
    static void lerp_pose(const float a[7], const float b[7],
                          float alpha, float out[7]);

    // 构建设备位姿序列 (分组 + 排序)
    std::vector<DevicePoseSeries> build_device_series();
};
