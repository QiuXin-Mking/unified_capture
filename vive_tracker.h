#pragma once
/*
 * vive_tracker.h — VIVE Tracker 3.0 位姿采集 Sensor
 *
 * 通过 libsurvive Low-Level Callback API 采集 Tracker 6DoF 位姿.
 * 输出 CSV 带 CLOCK_MONOTONIC 时间戳, 与视频/IMU/编码器对齐.
 */

#include "sensor.h"
#include "libsurvive/survive.h"

#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

// ============================================================
// 自动 unbind usbfs — 解决 LIBUSB_ERROR_BUSY
//
// 内核有时会把 VIVE Tracker HID 接口绑定到 usbfs,
// 这会阻止 libsurvive (通过 libusb) Claim 设备.
// 在 survive_init() 之前扫描 sysfs 并自动 unbind.
// ============================================================
// 返回 Tracker 的 sysfs 设备名 (如 "2-1.2"), 未找到返回 null
static const char* unbind_usbfs_for_vive(std::string& out_devname) {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return nullptr;

    struct dirent* entry;
    char path[512], buf[64];
    const char* found = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (name[0] == '.') continue;

        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        else buf[n] = '\0';
        if (strcmp(buf, "28de") != 0) continue;

        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        else buf[n] = '\0';
        if (strcmp(buf, "2300") != 0) continue;

        printf("[vive] found tracker (28de:2300) at %s\n", name);
        out_devname = name;
        found = name;

        for (int iface = 0; iface < 10; iface++) {
            snprintf(path, sizeof(path),
                     "/sys/bus/usb/devices/%s:1.%d/driver", name, iface);
            char link[256];
            n = readlink(path, link, sizeof(link) - 1);
            if (n <= 0) continue;
            link[n] = '\0';

            if (strstr(link, "usbfs")) {
                char ifname[256];
                snprintf(ifname, sizeof(ifname), "%s:1.%d", name, iface);
                printf("[vive] unbinding %s from usbfs\n", ifname);
                fd = open("/sys/bus/usb/drivers/usbfs/unbind", O_WRONLY);
                if (fd >= 0) {
                    ssize_t ignored = write(fd, ifname, strlen(ifname));
                    (void)ignored;
                    close(fd);
                }
            }
        }
        break;
    }
    closedir(dir);
    return found;
}

// 回绑 usbfs
static void rebind_usbfs(const std::string& devname) {
    if (devname.empty()) return;
    for (int iface = 0; iface < 10; iface++) {
        char ifname[256], path[512];
        snprintf(ifname, sizeof(ifname), "%s:1.%d", devname.c_str(), iface);
        // 只回绑当前未绑定驱动程序的接口
        snprintf(path, sizeof(path),
                 "/sys/bus/usb/devices/%s/driver", ifname);
        if (access(path, F_OK) == 0) continue;  // 已有驱动

        int fd = open("/sys/bus/usb/drivers/usbfs/bind", O_WRONLY);
        if (fd >= 0) {
            ssize_t ignored = write(fd, ifname, strlen(ifname));
            (void)ignored;
            close(fd);
        }
    }
}

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
        // 创建 CSV, session 根目录下与 encoder.csv 同级
        char path[256];
        snprintf(path, sizeof(path), "%s/tracker.jsonl",
                 session_dir_.c_str());
        fp_ = fopen(path, "w");
        if (!fp_) {
            fprintf(stderr, "[vive] 无法创建 %s\n", path);
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
            fclose(fp_);
            fp_ = nullptr;
            return;
        }

        s_instance_ = this;
        survive_install_pose_fn(ctx_, pose_callback);

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
                // 信号中断或其他错误 → 跳过 survive_close 避免 crash
                fprintf(stderr, "[vive] survive_poll error (%d), "
                        "skipping survive_close\n", ret);
                ctx_error_ = true;
                break;
            }
        }

        printf("[vive] collect done (%d polls, %llu poses)\n",
               poll_count, (unsigned long long)pose_count_);
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
    std::string dev_name_;       // sysfs 设备名, 用于回绑 usbfs
    uint64_t pose_count_ = 0;
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
        s_instance_->pose_count_++;

        uint64_t ts_us = elapsed_us();
        fprintf(s_instance_->fp_,
                "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\","
                "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
                "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
                (unsigned long long)ts_us,
                (unsigned long long)timecode,
                so->codename,
                pose->Pos[0], pose->Pos[1], pose->Pos[2],
                pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);

        // 每 100 条 flush 一次
        if (s_instance_->pose_count_ % 100 == 0)
            fflush(s_instance_->fp_);
    }
};
