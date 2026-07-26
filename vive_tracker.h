#pragma once
/*
 * vive_tracker.h — VIVE Tracker 3.0 位姿采集 Sensor
 *
 * 通过 libsurvive Low-Level Callback API 采集 Tracker 6DoF 位姿.
 * 支持多个 Tracker (左/右), 自动按 codename 分文件输出.
 * 输出 JSONL 带 CLOCK_MONOTONIC 时间戳, 与视频/IMU/编码器对齐.
 */

#include "sensor.h"
#include "vive_usb.h"
#include "libsurvive/survive.h"

#include <map>
#include <mutex>
#include <vector>

class ViveTrackerSensor : public Sensor {
public:
    ViveTrackerSensor(const std::string& session_dir,
                      int session_num,
                      const std::string& session_ts,
                      std::atomic<bool>& running)
        : Sensor("vive_tracker", running)
        , session_dir_(session_dir)
        , session_num_(session_num)
        , session_ts_(session_ts) {}

    ~ViveTrackerSensor() override = default;

protected:
    void setup() override {
        // 1. 扫描并 unbind 所有 VIVE Tracker
        dev_names_ = unbind_all_vive_trackers();
        printf("[vive] found %zu tracker(s)\n", dev_names_.size());

        // 2. 创建 tracker 子目录
        char path[256];
        snprintf(path, sizeof(path), "%s/tracker", session_dir_.c_str());
        mkdir_p(path, 0755);
        tracker_dir_ = path;

        // 3. survive_init 自动发现所有连接的 tracker
        char prog[] = "vive_tracker";
        char lh_count[] = "-l", lh_val[] = "2";
        char* dummy_argv[] = { prog, lh_count, lh_val, nullptr };
        ctx_ = survive_init(3, dummy_argv);
        if (!ctx_) {
            fprintf(stderr, "[vive] survive_init 失败 — "
                    "确认 Tracker 已通过 USB 连接, 且无其他进程占用\n");
            return;
        }

        // 4. 安装回调 (单实例指针, 回调中按 codename 动态创建文件)
        //    ★ 只装 pose 回调 (light/angle 回调不装, 参照 capture_vive.c)
        s_instance_ = this;
        survive_install_pose_fn(ctx_, pose_callback);

        printf("[vive] setup OK (%zu devices)\n", dev_names_.size());
        initialized_ = true;
    }

    void collect() override {
        if (!initialized_) return;

        int poll_count = 0;
        while (running_) {
            int ret = survive_poll(ctx_);
            poll_count++;
            if (ret != 0) {
                fprintf(stderr, "[vive] survive_poll error (%d) poll=%d\n", ret, poll_count);
                ctx_error_ = true;
                break;
            }
            if (poll_count % 500 == 0) {
                fprintf(stderr, "[vive] poll #%d OK (%llu poses, %llu angles, %llu lights)\n",
                        poll_count, (unsigned long long)pose_count_,
                        (unsigned long long)angle_count_, (unsigned long long)light_count_);
            }
            usleep(2000);
        }

        printf("[vive] collect done (%d polls, %llu poses, %llu angles, %llu lights)\n",
               poll_count, (unsigned long long)pose_count_,
               (unsigned long long)angle_count_, (unsigned long long)light_count_);
    }

    void teardown() override {
        // ★ survive_close 在双 tracker 时可能阻塞, 跳过显式关闭
        //    USB 设备和内存由进程退出时系统回收
        if (ctx_ && !ctx_error_) {
            // survive_close(ctx_);  // 可能阻塞, 不调用
            ctx_ = nullptr;
        }

        // 关闭所有动态创建的文件
        {
            std::lock_guard<std::mutex> lock(file_mutex_);
            for (auto& kv : tracker_files_) {
                if (kv.second) { fflush(kv.second); fclose(kv.second); }
            }
            tracker_files_.clear();
        }

        // 回绑所有 tracker 到 usbfs
        rebind_all_usbfs(dev_names_);

        s_instance_ = nullptr;
        printf("[vive] teardown OK\n");
    }

private:
    std::string session_dir_;
    int session_num_;
    std::string session_ts_;
    std::string tracker_dir_;                           // tracker/ 子目录
    SurviveContext* ctx_ = nullptr;
    std::vector<std::string> dev_names_;                // sysfs 设备名列表 (用于回绑)
    uint64_t pose_count_ = 0;
    uint64_t angle_count_ = 0;
    uint64_t light_count_ = 0;
    bool initialized_ = false;
    bool ctx_error_ = false;

    // ── Tracker 标签映射: 序列号 → 友好名称 ──
    //     编辑此表以匹配你的 tracker 序列号
    static std::map<std::string, std::string> tracker_labels_;

    // 动态文件: label → FILE* (pose + angle + light 统一写入同一文件)
    std::map<std::string, FILE*> tracker_files_;
    std::mutex file_mutex_;

    // 单实例指针 (libsurvive C 回调用)
    static inline ViveTrackerSensor* s_instance_ = nullptr;

    // 将序列号映射为友好标签
    static std::string resolve_label(const char* codename) {
        if (!codename) return "unknown";
        std::string key(codename);
        auto it = tracker_labels_.find(key);
        return (it != tracker_labels_.end()) ? it->second : key;
    }

    // ── 获取或创建 tracker 专属文件 (pose + angle + light 统一写入) ──
    FILE* get_or_create_tracker_file(const char* codename) {
        if (!codename) codename = "unknown";
        std::string label = resolve_label(codename);
        std::lock_guard<std::mutex> lock(file_mutex_);
        auto it = tracker_files_.find(label);
        if (it != tracker_files_.end()) return it->second;

        char path[256];
        snprintf(path, sizeof(path), "%s/tracker_%s-%s.jsonl",
                 tracker_dir_.c_str(), label.c_str(), session_ts_.c_str());
        FILE* fp = fopen(path, "w");
        if (fp) {
            tracker_files_[label] = fp;
            printf("[vive] '%s' → label '%s': %s\n", codename, label.c_str(), path);
        } else {
            fprintf(stderr, "[vive] 无法创建 %s\n", path);
        }
        return fp;
    }

    // ── libsurvive 位姿回调 ──
    static void pose_callback(SurviveObject* so, uint64_t timecode,
                              const SurvivePose* pose) {
        if (!s_instance_ || !so || !pose) return;
        s_instance_->pose_count_++;

        const char* codename = so->codename ? so->codename : "unknown";
        FILE* fp = s_instance_->get_or_create_tracker_file(codename);
        if (!fp) return;

        uint64_t ts_us = elapsed_us();
        fprintf(fp,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\","
                "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
                "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                codename,
                pose->Pos[0], pose->Pos[1], pose->Pos[2],
                pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);

        if (s_instance_->pose_count_ % 100 == 0)
            fflush(fp);
    }

    // ── libsurvive angle 回调 (Gen 1) ──
    static void angle_callback(SurviveObject* so, int sensor_id, int acode,
                               survive_timecode timecode, FLT length,
                               FLT angle, uint32_t lh) {
        if (!s_instance_ || !so) return;
        s_instance_->angle_count_++;

        const char* cn = so->codename ? so->codename : "unknown";
        FILE* fp = s_instance_->get_or_create_tracker_file(cn);
        if (!fp) return;

        uint64_t ts_us = elapsed_us();
        fprintf(fp,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\","
                "\"sensor\":%d,\"lh\":%u,\"acode\":%d,"
                "\"length\":%.6f,\"angle\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode, cn,
                sensor_id, lh, acode, (double)length, (double)angle);

        if (s_instance_->angle_count_ % 100 == 0)
            fflush(fp);
    }

    // ── libsurvive light 回调 (最底层 sensor hit 数据) ──
    static void light_callback(SurviveObject* so, int sensor_id, int acode,
                               int timeinsweep, survive_timecode timecode,
                               survive_timecode length, uint32_t lighthouse) {
        if (!s_instance_ || !so) return;
        s_instance_->light_count_++;

        const char* cn = so->codename ? so->codename : "unknown";
        FILE* fp = s_instance_->get_or_create_tracker_file(cn);
        if (!fp) return;

        uint64_t ts_us = elapsed_us();
        fprintf(fp,
                "{\"type\":\"light\",\"ts_us\":%llu,\"timecode\":%llu,"
                "\"codename\":\"%s\",\"sensor\":%d,\"lh\":%u,\"acode\":%d,"
                "\"timeinsweep\":%d,\"length\":%llu}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode, cn,
                sensor_id, lighthouse, acode,
                timeinsweep, (unsigned long long)length);

        if (s_instance_->light_count_ % 100 == 0)
            fflush(fp);
    }
};

// ── Tracker 序列号 → 友好标签 映射表 ──
//     将你的 VIVE Tracker 序列号填入此处:
//     LHR-43835B2F → tracker_left
//     LHR-XXXXXXXX → tracker_right
//     未匹配的序列号将原样使用作为文件名
inline std::map<std::string, std::string> ViveTrackerSensor::tracker_labels_ = {
    // {"T20", "tracker_left"},     // 2-1.1 LHR-43835B2F
    // {"???", "tracker_right"},    // 2-1.2 LHR-28A417A6
};
