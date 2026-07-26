/*
 * ViveTrackerSensor.cpp — VIVE Tracker 3.0 位姿采集 + 离线重采样
 *
 * 采集阶段: 全速写入 tracker_raw.jsonl + 内存缓冲
 * 重采样阶段: teardown 时从缓冲重采样到固定 Hz, 写 tracker.jsonl
 *
 * 重采样算法:
 *   - 按设备分组, timecode 排序
 *   - 找到所有设备时间范围的并集
 *   - 每个 10ms 网格点, 为每个设备取最近邻/插值位姿
 *   - 输出一行 JSON, 所有设备时间对齐
 */

#include "ViveTrackerSensor.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// ============================================================
// 构造 / 析构
// ============================================================
ViveTrackerSensor::ViveTrackerSensor(
        const std::string& session_dir,
        int session_num,
        std::atomic<bool>& running,
        const ResampleConfig& cfg)
    : Sensor("vive_tracker", running)
    , session_dir_(session_dir)
    , session_num_(session_num)
    , resample_cfg_(cfg) {
    raw_buffer_.reserve(kBufferReserve);
    if (cfg.enable) {
        resample_cfg_.tc_interval = (uint64_t)(48000000.0 / cfg.target_hz);
    }
}

ViveTrackerSensor::~ViveTrackerSensor() = default;

// ============================================================
// setup
// ============================================================
void ViveTrackerSensor::setup() {
    // 创建输出文件
    char path[256];
    snprintf(path, sizeof(path), "%s/tracker_raw.jsonl",
             session_dir_.c_str());
    fp_raw_ = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/tracker_angle.jsonl",
             session_dir_.c_str());
    fp_angle_ = fopen(path, "w");
    if (!fp_raw_ || !fp_angle_) {
        fprintf(stderr, "[vive] 无法创建输出文件\n");
        return;
    }

    // 自动 unbind usbfs
    unbind_usbfs_for_vive(dev_name_);

    // 初始化 libsurvive — 传递命令行参数
    // 使用 --force-calibrate --force-ootx 保证冷启动可靠
    const char* args[] = {
        "vive_tracker",
        "-l", "2",
        "--force-calibrate",
        "--force-ootx",
        nullptr
    };
    int argc = 7;
    char** argv = const_cast<char**>(args);

    ctx_ = survive_init(argc, argv);
    if (!ctx_) {
        fprintf(stderr, "[vive] survive_init 失败 — "
                "确认 Tracker 已通过 USB 连接\n");
        return;
    }

    s_instance_ = this;
    survive_install_pose_fn(ctx_, pose_callback);
    survive_install_angle_fn(ctx_, angle_callback);
    survive_install_light_fn(ctx_, light_callback);

    printf("[vive] setup OK (resample=%s, target=%.0fHz)\n",
           resample_cfg_.enable ? "on" : "off",
           resample_cfg_.target_hz);
    initialized_ = true;
}

// ============================================================
// collect — 全速采集 + 内存缓冲
// ============================================================
void ViveTrackerSensor::collect() {
    if (!initialized_) return;

    int poll_count = 0;
    while (running_) {
        int ret = survive_poll(ctx_);
        poll_count++;
        if (ret != 0) {
            fprintf(stderr, "[vive] survive_poll error (%d) poll=%d\n",
                    ret, poll_count);
            ctx_error_ = true;
            break;
        }
        if (poll_count % 500 == 0) {
            fprintf(stderr, "[vive] poll #%d OK (%llu poses buf=%zu)\n",
                    poll_count,
                    (unsigned long long)pose_count_,
                    raw_buffer_.size());
        }
        usleep(2000);  // 2ms 避免 CPU 空转
    }

    printf("[vive] collect done (%d polls, %llu poses, %llu angles, "
           "%llu lights, buffer=%zu)\n",
           poll_count,
           (unsigned long long)pose_count_,
           (unsigned long long)angle_count_,
           (unsigned long long)light_count_,
           raw_buffer_.size());
}

// ============================================================
// teardown — 重采样 + 写文件 + 清理
// ============================================================
void ViveTrackerSensor::teardown() {
    // 关闭 libsurvive
    if (ctx_) {
        if (!ctx_error_) {
            survive_close(ctx_);
        }
        ctx_ = nullptr;
    }

    // 关闭原始输出
    if (fp_raw_) {
        fflush(fp_raw_);
        fclose(fp_raw_);
        fp_raw_ = nullptr;
    }
    if (fp_angle_) {
        fflush(fp_angle_);
        fclose(fp_angle_);
        fp_angle_ = nullptr;
    }

    // ★ 离线重采样
    if (resample_cfg_.enable && !raw_buffer_.empty()) {
        printf("[vive] 开始重采样... (%zu raw poses → %.0f Hz)\n",
               raw_buffer_.size(), resample_cfg_.target_hz);
        resample_and_write();
    }

    // 回绑 usbfs
    rebind_usbfs(dev_name_);
    s_instance_ = nullptr;

    printf("[vive] teardown OK\n");
}

// ============================================================
// pose_callback — 位姿回调 (跑在 survive_poll() 线程内)
// ============================================================
void ViveTrackerSensor::pose_callback(SurviveObject* so,
                                       uint64_t timecode,
                                       const SurvivePose* pose) {
    if (!s_instance_ || !s_instance_->fp_raw_) return;
    if (!so || !pose) return;
    s_instance_->pose_count_++;

    uint64_t ts_us = elapsed_us();
    const char* name = so->codename ? so->codename : "unknown";

    // 写入 raw JSONL
    fprintf(s_instance_->fp_raw_,
            "{\"ts_us\":%llu,\"tc\":%llu,\"dev\":\"%s\","
            "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
            "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
            (unsigned long long)ts_us,
            (unsigned long long)timecode,
            name,
            pose->Pos[0], pose->Pos[1], pose->Pos[2],
            pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);

    // 缓冲到内存 (供重采样)
    if (s_instance_->resample_cfg_.enable) {
        PoseRecord rec;
        rec.timecode = timecode;
        strncpy(rec.codename, name, sizeof(rec.codename) - 1);
        rec.codename[sizeof(rec.codename) - 1] = '\0';
        rec.x  = (float)pose->Pos[0];
        rec.y  = (float)pose->Pos[1];
        rec.z  = (float)pose->Pos[2];
        rec.qw = (float)pose->Rot[0];
        rec.qx = (float)pose->Rot[1];
        rec.qy = (float)pose->Rot[2];
        rec.qz = (float)pose->Rot[3];
        s_instance_->raw_buffer_.push_back(rec);
    }

    if (s_instance_->pose_count_ % 100 == 0)
        fflush(s_instance_->fp_raw_);
}

// ============================================================
// angle_callback
// ============================================================
void ViveTrackerSensor::angle_callback(SurviveObject* so, int sensor_id,
                                        int acode, survive_timecode timecode,
                                        FLT length, FLT angle, uint32_t lh) {
    if (!s_instance_ || !s_instance_->fp_angle_) return;
    if (!so) return;
    s_instance_->angle_count_++;

    uint64_t ts_us = elapsed_us();
    fprintf(s_instance_->fp_angle_,
            "{\"ts_us\":%llu,\"tc\":%llu,\"sensor\":%d,\"lh\":%u,"
            "\"acode\":%d,\"length\":%.6f,\"angle\":%.6f}\n",
            (unsigned long long)ts_us,
            (unsigned long long)timecode,
            sensor_id, lh, acode, (double)length, (double)angle);

    if (s_instance_->angle_count_ % 100 == 0)
        fflush(s_instance_->fp_angle_);
}

// ============================================================
// light_callback
// ============================================================
void ViveTrackerSensor::light_callback(SurviveObject* so, int sensor_id,
                                        int acode, int timeinsweep,
                                        survive_timecode timecode,
                                        survive_timecode length,
                                        uint32_t lighthouse) {
    if (!s_instance_ || !s_instance_->fp_angle_) return;
    if (!so) return;
    s_instance_->light_count_++;

    uint64_t ts_us = elapsed_us();
    fprintf(s_instance_->fp_angle_,
            "{\"type\":\"light\",\"ts_us\":%llu,\"tc\":%llu,"
            "\"sensor\":%d,\"lh\":%u,\"acode\":%d,"
            "\"timeinsweep\":%d,\"length\":%llu}\n",
            (unsigned long long)ts_us,
            (unsigned long long)timecode,
            sensor_id, lighthouse, acode,
            timeinsweep, (unsigned long long)length);

    if (s_instance_->light_count_ % 100 == 0)
        fflush(s_instance_->fp_angle_);
}

// ============================================================
// build_device_series — 按设备分组 + timecode 排序
// ============================================================
std::vector<DevicePoseSeries> ViveTrackerSensor::build_device_series() {
    // 分组
    std::unordered_map<std::string, DevicePoseSeries> map;
    for (const auto& rec : raw_buffer_) {
        auto& ds = map[rec.codename];
        ds.name = rec.codename;
        ds.poses.push_back(rec);
        if (rec.timecode < ds.t_min) ds.t_min = rec.timecode;
        if (rec.timecode > ds.t_max) ds.t_max = rec.timecode;
    }

    // 排序
    std::vector<DevicePoseSeries> result;
    result.reserve(map.size());
    for (auto& [name, ds] : map) {
        std::sort(ds.poses.begin(), ds.poses.end(),
                  [](const PoseRecord& a, const PoseRecord& b) {
                      return a.timecode < b.timecode;
                  });
        result.push_back(std::move(ds));
    }

    // 按名字排序 (保证输出顺序稳定)
    std::sort(result.begin(), result.end(),
              [](const DevicePoseSeries& a, const DevicePoseSeries& b) {
                  return a.name < b.name;
              });

    return result;
}

// ============================================================
// nearest — 二分查找最近邻
// ============================================================
const PoseRecord* ViveTrackerSensor::nearest(
        const std::vector<PoseRecord>& poses,
        uint64_t target_tc) {

    if (poses.empty()) return nullptr;

    // 二分查找第一个 >= target 的位置
    auto it = std::lower_bound(poses.begin(), poses.end(), target_tc,
        [](const PoseRecord& r, uint64_t t) { return r.timecode < t; });

    if (it == poses.begin()) {
        return &poses.front();  // 都在 target 之后
    }
    if (it == poses.end()) {
        return &poses.back();   // 都在 target 之前
    }

    // 比较 it-1 和 it, 选更近的
    const auto& before = *(it - 1);
    const auto& after  = *it;
    uint64_t d_before = target_tc - before.timecode;
    uint64_t d_after  = after.timecode - target_tc;

    return (d_before <= d_after) ? &before : &after;
}

// ============================================================
// lerp_pose — 7 元素位姿线性插值
// ============================================================
void ViveTrackerSensor::lerp_pose(const float a[7], const float b[7],
                                   float alpha, float out[7]) {
    // alpha=0 → a, alpha=1 → b
    for (int i = 0; i < 7; i++) {
        out[i] = a[i] + alpha * (b[i] - a[i]);
    }
    // 四元数归一化 (线性插值后不再是单位四元数)
    float norm = std::sqrt(out[3]*out[3] + out[4]*out[4] +
                           out[5]*out[5] + out[6]*out[6]);
    if (norm > 1e-9f) {
        out[3] /= norm; out[4] /= norm;
        out[5] /= norm; out[6] /= norm;
    }
}

// ============================================================
// interpolate — 线性插值
// ============================================================
bool ViveTrackerSensor::interpolate(
        const std::vector<PoseRecord>& poses,
        uint64_t target_tc,
        float out[7]) {

    if (poses.size() < 2) return false;

    // 二分查找
    auto it = std::lower_bound(poses.begin(), poses.end(), target_tc,
        [](const PoseRecord& r, uint64_t t) { return r.timecode < t; });

    if (it == poses.begin() || it == poses.end()) {
        return false;  // 外推不行
    }

    const auto& before = *(it - 1);
    const auto& after  = *it;

    float alpha = (float)(target_tc - before.timecode) /
                  (float)(after.timecode - before.timecode);
    if (alpha < 0.0f || alpha > 1.0f) return false;

    float a[7] = {before.x, before.y, before.z,
                  before.qw, before.qx, before.qy, before.qz};
    float b[7] = {after.x, after.y, after.z,
                  after.qw, after.qx, after.qy, after.qz};
    lerp_pose(a, b, alpha, out);
    return true;
}

// ============================================================
// resample_and_write — 离线重采样主逻辑
// ============================================================
void ViveTrackerSensor::resample_and_write() {
    auto devices = build_device_series();
    if (devices.empty()) {
        fprintf(stderr, "[vive] resample: no devices\n");
        return;
    }

    // 找全局时间范围 (所有设备的并集)
    uint64_t t_min = UINT64_MAX, t_max = 0;
    for (const auto& ds : devices) {
        if (ds.t_min < t_min) t_min = ds.t_min;
        if (ds.t_max > t_max) t_max = ds.t_max;
    }
    if (t_min >= t_max) {
        fprintf(stderr, "[vive] resample: empty time range\n");
        return;
    }

    // 设备信息
    fprintf(stderr, "[vive] resample: %zu devices, tc range %llu → %llu "
            "(%.1fs), interval=%llu ticks (%.1fms)\n",
            devices.size(),
            (unsigned long long)t_min, (unsigned long long)t_max,
            (t_max - t_min) / 48000000.0,
            (unsigned long long)resample_cfg_.tc_interval,
            resample_cfg_.tc_interval / 48000000.0 * 1000.0);

    // 打开输出文件
    char path[256];
    snprintf(path, sizeof(path), "%s/tracker.jsonl",
             session_dir_.c_str());
    FILE* fp_out = fopen(path, "w");
    if (!fp_out) {
        fprintf(stderr, "[vive] resample: 无法创建 %s\n", path);
        return;
    }

    const char* method = resample_cfg_.use_interp ? "lerp" : "nearest";
    uint64_t total = 0, skipped = 0;

    // 遍历时间网格
    uint64_t t = t_min + resample_cfg_.tc_interval;  // 从第一个网格点开始
    while (t <= t_max) {

        fprintf(fp_out, "{\"tc\":%llu,\"method\":\"%s\"",
                (unsigned long long)t, method);

        bool all_ok = true;
        for (const auto& ds : devices) {
            float pose[7];
            bool ok = false;

            if (resample_cfg_.use_interp) {
                ok = interpolate(ds.poses, t, pose);
            } else {
                const auto* rec = nearest(ds.poses, t);
                if (rec) {
                    pose[0] = rec->x;  pose[1] = rec->y;
                    pose[2] = rec->z;
                    pose[3] = rec->qw; pose[4] = rec->qx;
                    pose[5] = rec->qy; pose[6] = rec->qz;
                    ok = true;
                }
            }

            if (ok) {
                fprintf(fp_out,
                        ",\"%s_x\":%.6f,\"%s_y\":%.6f,\"%s_z\":%.6f,"
                        "\"%s_qw\":%.6f,\"%s_qx\":%.6f,"
                        "\"%s_qy\":%.6f,\"%s_qz\":%.6f",
                        ds.name.c_str(), pose[0],
                        ds.name.c_str(), pose[1],
                        ds.name.c_str(), pose[2],
                        ds.name.c_str(), pose[3],
                        ds.name.c_str(), pose[4],
                        ds.name.c_str(), pose[5],
                        ds.name.c_str(), pose[6]);
            } else {
                all_ok = false;
                break;
            }
        }

        fprintf(fp_out, "}\n");

        if (all_ok) {
            total++;
        } else {
            skipped++;
        }

        t += resample_cfg_.tc_interval;
    }

    fflush(fp_out);
    fclose(fp_out);

    double duration = (t_max - t_min) / 48000000.0;
    double actual_hz = total / duration;

    printf("[vive] resample done: %llu frames, %.1f Hz, "
           "%llu skipped, file=%s\n",
           (unsigned long long)total, actual_hz,
           (unsigned long long)skipped, path);
}
