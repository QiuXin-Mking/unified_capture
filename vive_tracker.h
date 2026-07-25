#pragma once
/*
 * vive_tracker.h — VIVE Tracker 3.0 位姿采集 Sensor
 *
 * 通过 libsurvive Low-Level Callback API 采集 Tracker 6DoF 位姿.
 * 输出 CSV 带 CLOCK_MONOTONIC 时间戳, 与视频/IMU/编码器对齐.
 */

#include "sensor.h"
#include "vive_usb.h"
#include "libsurvive/survive.h"

class ViveTrackerSensor : public Sensor {
public:
    ViveTrackerSensor(const std::string& session_dir,
                      int session_num,
                      std::atomic<bool>& running)
        : Sensor("vive_tracker", running)
        , session_dir_(session_dir)
        , session_num_(session_num) {}

    ~ViveTrackerSensor() override = default;

protected:
    void setup() override {
        // 创建 tracker.jsonl (pose) 和 tracker_angle.jsonl (angle)
        char path[256];
        snprintf(path, sizeof(path), "%s/tracker.jsonl",
                 session_dir_.c_str());
        fp_ = fopen(path, "w");
        snprintf(path, sizeof(path), "%s/tracker_angle.jsonl",
                 session_dir_.c_str());
        fp_angle_ = fopen(path, "w");
        if (!fp_ || !fp_angle_) {
            fprintf(stderr, "[vive] 无法创建输出文件\n");
            return;
        }

        // 先自动 unbind usbfs (解决 LIBUSB_ERROR_BUSY), 记下设备名用于回绑
        unbind_usbfs_for_vive(dev_name_);

        // survive_init 会解析命令行参数, 传假的 argv[0] 即可
        char prog[] = "vive_tracker";
        char* dummy_argv[] = { prog, nullptr };
        ctx_ = survive_init(1, dummy_argv);
        if (!ctx_) {
            fprintf(stderr, "[vive] survive_init 失败 — "
                    "确认 Tracker 已通过 USB 连接, 且无其他进程占用\n");
            return;
        }

        s_instance_ = this;
        survive_install_pose_fn(ctx_, pose_callback);
        survive_install_angle_fn(ctx_, angle_callback);         // Gen 1
        // Gen 2 callbacks (not needed for LH gen 1 system)
        survive_install_light_fn(ctx_, light_callback);         // raw sensor hit

        printf("[vive] setup OK\n");
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
                fprintf(stderr, "[vive] poll #%d OK (%llu poses)\n",
                        poll_count, (unsigned long long)pose_count_);
            }
            usleep(2000);
        }

        printf("[vive] collect done (%d polls, %llu poses, %llu angles, %llu lights)\n",
               poll_count, (unsigned long long)pose_count_,
               (unsigned long long)angle_count_, (unsigned long long)light_count_);
    }

    void teardown() override {
        if (ctx_) {
            if (!ctx_error_) {
                survive_close(ctx_);
            }
            ctx_ = nullptr;
        }
        if (fp_) {
            fflush(fp_);
            fclose(fp_);
            fp_ = nullptr;
        }
        if (fp_angle_) {
            fflush(fp_angle_);
            fclose(fp_angle_);
            fp_angle_ = nullptr;
        }
        // 回绑 usbfs — 恢复驱动状态
        rebind_usbfs(dev_name_);

        s_instance_ = nullptr;
        printf("[vive] teardown OK\n");
    }

private:
    std::string session_dir_;
    int session_num_;
    SurviveContext* ctx_ = nullptr;
    FILE* fp_ = nullptr;
    FILE* fp_angle_ = nullptr;
    std::string dev_name_;       // sysfs 设备名, 用于回绑 usbfs
    uint64_t pose_count_ = 0;
    uint64_t angle_count_ = 0;
    uint64_t light_count_ = 0;
    bool initialized_ = false;
    bool ctx_error_ = false;

    // 只有一个 VIVE Tracker, 用静态实例指针给回调用
    static inline ViveTrackerSensor* s_instance_ = nullptr;

    // libsurvive 位姿回调 (跑在 survive_poll() 线程内)
    // timecode: Tracker 内部 48-tick 时间戳 (survive_long_timecode = uint64_t)
    // pose:    位置 (米) + 四元数
    static void pose_callback(SurviveObject* so, uint64_t timecode,
                              const SurvivePose* pose) {
        if (!s_instance_ || !s_instance_->fp_) return;
        if (!so || !pose) return;  // NULL 保护
        s_instance_->pose_count_++;

        uint64_t ts_us = elapsed_us();
        const char* name = so->codename ? so->codename : "unknown";
        fprintf(s_instance_->fp_,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\","
                "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
                "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                name,
                pose->Pos[0], pose->Pos[1], pose->Pos[2],
                pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);

        // 每 100 条 flush 一次
        if (s_instance_->pose_count_ % 100 == 0)
            fflush(s_instance_->fp_);
    }

    // libsurvive angle 回调 — 单基站即可, 不依赖 disambiguator
    static void angle_callback(SurviveObject* so, int sensor_id, int acode,
                               survive_timecode timecode, FLT length,
                               FLT angle, uint32_t lh) {
        if (!s_instance_ || !s_instance_->fp_angle_) return;
        if (!so) return;
        s_instance_->angle_count_++;

        uint64_t ts_us = elapsed_us();
        fprintf(s_instance_->fp_angle_,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"sensor\":%d,\"lh\":%u,"
                "\"acode\":%d,\"length\":%.6f,\"angle\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                sensor_id, lh, acode, (double)length, (double)angle);

        if (s_instance_->angle_count_ % 100 == 0)
            fflush(s_instance_->fp_angle_);
    }

    // libsurvive sweep_angle 回调 (Gen 2 base station)
    static void sweep_angle_cb(SurviveObject* so, survive_channel channel,
                               int sensor_id, survive_timecode timecode,
                               int8_t plane, FLT angle) {
        if (!s_instance_ || !s_instance_->fp_angle_) return;
        if (!so) return;
        s_instance_->angle_count_++;

        uint64_t ts_us = elapsed_us();
        fprintf(s_instance_->fp_angle_,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"channel\":%d,\"sensor\":%d,"
                "\"plane\":%d,\"angle\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                (int)channel, sensor_id, (int)plane, (double)angle);

        if (s_instance_->angle_count_ % 100 == 0)
            fflush(s_instance_->fp_angle_);
    }

    // libsurvive light 回调 — 最底层, 每次 sensor 被 sweep 击中
    static void light_callback(SurviveObject* so, int sensor_id, int acode,
                               int timeinsweep, survive_timecode timecode,
                               survive_timecode length, uint32_t lighthouse) {
        if (!s_instance_ || !s_instance_->fp_angle_) return;
        if (!so) return;
        s_instance_->light_count_++;

        uint64_t ts_us = elapsed_us();
        fprintf(s_instance_->fp_angle_,
                "{\"type\":\"light\",\"ts_us\":%llu,\"timecode\":%llu,"
                "\"sensor\":%d,\"lh\":%u,\"acode\":%d,"
                "\"timeinsweep\":%d,\"length\":%llu}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                sensor_id, lighthouse, acode,
                timeinsweep, (unsigned long long)length);

        if (s_instance_->light_count_ % 100 == 0)
            fflush(s_instance_->fp_angle_);
    }
};
