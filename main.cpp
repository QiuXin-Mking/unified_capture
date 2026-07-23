/*
 * main.cpp — 统一采集系统主入口
 *
 * 4 路摄像头 (TSTC SDK + MPP H.265) + 4 路 IMU (异步解码) + AS5600 磁编码器
 * GPIO 按钮 启动/停止, 统一 CLOCK_MONOTONIC 时间基
 *
 * 用法:
 *   ./unified_capture [output_prefix]
 *
 * 输出:
 *   {prefix}/session_001/
 *     jhh2_left/   001.mkv  001_imu.csv
 *     jhh2_right/  001.mkv  001_imu.csv
 *     jhh04/       001.mkv  001_imu.csv
 *     jhh02/       001.mkv  001_imu.csv
 *     encoder.csv
 *
 * 鲁棒性:
 *   - 按 USB VID/PID 自动匹配设备, 不管插入数量/顺序
 *   - 缺设备自动禁用, 不会崩溃或误配
 *   - AS5600 探测失败自动跳过
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <gpiod.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include "USBCam_API.h"
}

#include "camera_config.h"
#include "sensor.h"
#include "video_sensor.h"
#include "imu_sensor.h"
#include "encoder_sensor.h"
#include "vive_tracker.h"

// ============================================================
// 全局状态
// ============================================================
struct timespec g_t0;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_session_running{false};

static void sig_handler(int) {
    g_running = false;
    g_session_running = false;
}

// ============================================================
// 摄像头配置
//
// 按 USB VID/PID 自动匹配, 不依赖固定索引.
// group_order: 同一 VID/PID 组内的设备编号 (0=第一台, 1=第二台)
//
// 已知 USB ID:
//   JHH2 系列  → 1bcf:2d50  (3840×1200)
//   六目系列    → 1bcf:2d51  (3104×480)
// ============================================================
#define JHH2_VID  0x1bcf
#define JHH2_PID  0x2d50
#define SIX_VID   0x1bcf
#define SIX_PID   0x2d51

struct CamEntry {
    CameraConfig cfg;
    bool enabled = true;
    v4l2_dev_sys_data_t* dev_ptr = nullptr;  // 匹配到的设备 (resolve 后填写)
};

// CameraConfig: name, vid, pid, group_order, width, height, fps, bitrate, gop, has_imu, imu_orientation, output_h265, output_y8
static CamEntry CAMS[] = {
    // JHH2 双目: H.265 + Y8
    {{"jhh2_left",  JHH2_VID, JHH2_PID, 0, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
    {{"jhh2_right", JHH2_VID, JHH2_PID, 1, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
    // JHH04 四目: 仅 Y8 (不给 SLAM 浪费 H.265 编码)
    {{"jhh04",      SIX_VID,  SIX_PID,  0, 3104,  480, 30,  4000000, 30, true,  ImuOrientation::VERTICAL_LEFT,  false, true},  true, nullptr},
    // JHH02 双目 (六目模组内的 JHH2 通道, 使用 JHH2 VID/PID): H.265 + Y8
    {{"jhh02",      JHH2_VID, JHH2_PID, 2, 3104,  480, 30,  4000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
};
static const int N_CAMS = sizeof(CAMS) / sizeof(CAMS[0]);

// AS5600 编码器配置
static const char*  ENC_I2C_PATH    = "/dev/i2c-6";
static const int    ENC_I2C_ADDR    = 0x36;
static const int    ENC_INTERVAL_US = 10000;   // 100Hz

// GPIO
static const char*  GPIO_CHIP     = "/dev/gpiochip2";
static const int    GPIO_BTN_LINE = 8;  // gpio 72 = gpiochip2 line 8
#define LED_PATH      "/sys/class/leds/sys_led"

// ============================================================
// LED 控制 (sysfs)
// ============================================================
static void led_set(int brightness) {
    int fd = open(LED_PATH "/brightness", O_WRONLY);
    if (fd < 0) return;
    char v = brightness ? '1' : '0';
    ssize_t ignored = write(fd, &v, 1);
    (void)ignored;
    close(fd);
}

static void led_disable_trigger() {
    int fd = open(LED_PATH "/trigger", O_WRONLY);
    if (fd < 0) return;
    ssize_t ignored = write(fd, "none", 4);
    (void)ignored;
    close(fd);
}

// ============================================================
// 设备扫描 (按 VID/PID 分组)
// ============================================================
static void scan_devices() {
    // JHH2 组
    {
        v4l2_dev_sys_data_t* devs = nullptr;
        int n = TST_USBCam_DEVICE_FIND_ID(&devs, JHH2_VID, JHH2_PID);
        printf("JHH2 组 [%04x:%04x]: %d device(s)\n", JHH2_VID, JHH2_PID, n);
        for (int i = 0; i < n; i++) {
            printf("  [group:%d] %s %s  (%s)\n",
                   i, devs[i].iManufacturer, devs[i].iProduct, devs[i].Device_Path);
        }
    }
    // 六目组
    {
        v4l2_dev_sys_data_t* devs = nullptr;
        int n = TST_USBCam_DEVICE_FIND_ID(&devs, SIX_VID, SIX_PID);
        printf("六目组 [%04x:%04x]: %d device(s)\n", SIX_VID, SIX_PID, n);
        for (int i = 0; i < n; i++) {
            printf("  [group:%d] %s %s  (%s)\n",
                   i, devs[i].iManufacturer, devs[i].iProduct, devs[i].Device_Path);
        }
    }
    printf("\nCamera mapping (CAMS → device):\n");
    for (int i = 0; i < N_CAMS; i++) {
        auto& cam = CAMS[i];
        int go = cam.cfg.group_order;
        printf("  %-12s → VID/PID [%04x:%04x] group_order=%d\n",
               cam.cfg.name, cam.cfg.vid, cam.cfg.pid, go);
    }
}

// ============================================================
// VID/PID 分组缓存 — 避免对同一 VID/PID 重复扫描
// ============================================================
struct VidPidGroup {
    uint16_t vid, pid;
    v4l2_dev_sys_data_t* devs;
    int count;
};

// ============================================================
// 按 VID/PID 自动匹配摄像头设备
// 如果某台摄像头没有对应的物理设备, 自动禁用
// ============================================================
static int resolve_camera_devices() {
    std::vector<VidPidGroup> groups;

    for (int i = 0; i < N_CAMS; i++) {
        auto& cam = CAMS[i];
        if (!cam.enabled) continue;

        uint16_t vid = cam.cfg.vid;
        uint16_t pid = cam.cfg.pid;

        // 查找或创建 VID/PID 组缓存
        VidPidGroup* grp = nullptr;
        for (auto& g : groups) {
            if (g.vid == vid && g.pid == pid) { grp = &g; break; }
        }
        if (!grp) {
            VidPidGroup g;
            g.vid = vid; g.pid = pid;
            g.count = TST_USBCam_DEVICE_FIND_ID(&g.devs, vid, pid);
            groups.push_back(g);
            grp = &groups.back();
        }

        int order = cam.cfg.group_order;
        if (order >= grp->count) {
            cam.enabled = false;
            cam.dev_ptr = nullptr;
            fprintf(stderr, "WARN: %s [%04x:%04x group_order=%d] not found "
                    "(group has %d device(s)) → disabled\n",
                    cam.cfg.name, vid, pid, order, grp->count);
            continue;
        }

        cam.dev_ptr = &grp->devs[order];
        printf("  %-12s → [%04x:%04x group:%d] %s  %dx%d@%d  IMU=%c\n",
               cam.cfg.name, vid, pid, order,
               cam.dev_ptr->Device_Path,
               cam.cfg.width, cam.cfg.height, cam.cfg.fps,
               cam.cfg.has_imu ? 'Y' : 'N');
    }

    // 统计实际可用的摄像头数
    int active = 0;
    for (int i = 0; i < N_CAMS; i++) {
        if (CAMS[i].enabled) active++;
    }
    return active;
}

// ============================================================
// Session 目录
// ============================================================
static std::string make_session_dir(const std::string& prefix, int num) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/session_%03d", prefix.c_str(), num);
    std::string dir(buf);
    mkdir_p(buf, 0755);
    for (int i = 0; i < N_CAMS; i++) {
        if (!CAMS[i].enabled) continue;
        snprintf(buf, sizeof(buf), "%s/%s", dir.c_str(), CAMS[i].cfg.name);
        mkdir_p(buf, 0755);
    }
    return dir;
}

static std::string default_prefix() {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    snprintf(buf, sizeof(buf), "record_%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// ============================================================
// 运行一次 Session
// ============================================================
static void run_session(const std::string& ses_dir, int session_num,
                        bool use_imu, bool use_as5600, bool use_vive,
                        gpiod_line* btn = nullptr) {
    printf("\n>>> Session %d START <<<\n", session_num);

    // 统一时间纪元
    clock_gettime(CLOCK_MONOTONIC, &g_t0);

    std::vector<Sensor*> sensors;

    // ---- 创建 VideoSensor + ImuSensor ----
    for (int i = 0; i < N_CAMS; i++) {
        if (!CAMS[i].enabled) continue;

        auto& cam = CAMS[i];
        auto* vs = new VideoSensor(cam.cfg, ses_dir, *cam.dev_ptr,
                                   session_num, g_session_running);
        sensors.push_back(vs);

        if (use_imu && cam.cfg.has_imu) {
            sensors.push_back(new ImuSensor(
                std::string(cam.cfg.name), ses_dir,
                vs->imu_queue(), session_num,
                cam.cfg.imu_orientation, g_session_running));
        }
    }

    // ---- AS5600 ----
    if (use_as5600) {
        sensors.push_back(new EncoderSensor(
            ENC_I2C_PATH, ENC_I2C_ADDR,
            ses_dir, session_num, ENC_INTERVAL_US, g_session_running));
    }

    // ---- VIVE Tracker 3.0 ----
    if (use_vive) {
        sensors.push_back(new ViveTrackerSensor(
            ses_dir, session_num, g_session_running));
    }

    // ---- Launch: barrier 同步 ----
    if (sensors.empty()) {
        fprintf(stderr, "WARN: no sensors to launch, skipping session\n");
        return;
    }
    size_t n = sensors.size() + 1;
    SimpleBarrier gate(n);
    for (auto* s : sensors) s->launch(gate);
    gate.arrive_and_wait();
    printf(">>> ALL SENSORS GO <<<\n");

    // ---- 等停止信号 (按钮 或 Ctrl-C) ----
    while (g_session_running) {
        if (btn) {
            struct timespec timeout = {0, 50000000};  // 50ms
            int ret = gpiod_line_event_wait(btn, &timeout);
            if (ret > 0) {
                gpiod_line_event ev;
                gpiod_line_event_read(btn, &ev);
                if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    g_session_running = false;
                }
            }
        } else {
            usleep(50000);
        }
    }

    printf("\n>>> Session %d STOP <<<\n", session_num);

    // ---- Join + Cleanup ----
    for (auto* s : sensors) { s->join(); delete s; }
    printf(">>> Session %d DONE <<<\n\n", session_num);
}

// ============================================================
static void print_usage(const char* prog) {
    printf(
        "Usage: %s [OPTIONS] [output_prefix]\n\n"
        "Unified Capture System — 4 cameras + IMU + AS5600\n\n"
        "Options:\n"
        "  --scan         Scan TSTC devices (by VID/PID groups)\n"
        "  --no-gpio      No GPIO: auto-start, Ctrl-C to stop & exit\n"
        "  --no-as5600    Disable AS5600\n"
        "  --no-imu       Disable IMU decoding\n"
        "  --no-vive      Disable VIVE Tracker 3.0\n"
        "  --single       Run one session only (GPIO mode)\n"
        "  -h, --help     This help\n\n"
        "GPIO mode: press button to start/stop, Ctrl-C to exit\n"
        "No-GPIO mode: starts immediately, Ctrl-C to stop\n\n"
        "Examples:\n"
        "  %s --scan\n"
        "  %s my_experiment\n"
        "  %s --no-gpio test_run\n", prog, prog, prog, prog);
}

// ============================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    bool use_gpio      = true;
    bool use_as5600    = true;
    bool use_imu       = true;
    bool use_vive      = true;
    bool single_shot   = false;
    std::string prefix;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scan"))          { scan_devices(); return 0; }
        else if (!strcmp(argv[i], "--no-gpio"))  { use_gpio = false; }
        else if (!strcmp(argv[i], "--no-as5600")){ use_as5600 = false; }
        else if (!strcmp(argv[i], "--no-imu"))   { use_imu = false; }
        else if (!strcmp(argv[i], "--no-vive"))  { use_vive = false; }
        else if (!strcmp(argv[i], "--single"))   { single_shot = true; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            { print_usage(argv[0]); return 0; }
        else if (argv[i][0] != '-')              { prefix = argv[i]; }
    }
    if (prefix.empty()) prefix = default_prefix();

    // ---- 按 VID/PID 自动匹配摄像头设备 ----
    int active = resolve_camera_devices();
    if (active <= 0) {
        fprintf(stderr, "ERROR: No cameras found via TSTC SDK\n");
        return 1;
    }

    printf("\n=== Unified Capture (%d camera(s) active) ===\n", active);
    printf("Output: %s/\n", prefix.c_str());

    led_disable_trigger();
    led_set(0);

    // ============================================================
    // 模式分支
    // ============================================================
    if (!use_gpio) {
        // ---- 无 GPIO 模式: 启动即录, Ctrl-C 停止退出 ----
        printf("\nRecording... Press Ctrl-C to stop.\n");
        g_session_running = true;

        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);

        printf("=== Unified Capture Exit ===\n");
        return 0;
    }

    // ---- GPIO 模式: 按钮切换开始/停止, Ctrl-C 退出 ----
    gpiod_chip* chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        fprintf(stderr, "gpiod_chip_open(%s) failed, fallback --no-gpio\n", GPIO_CHIP);
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_running = true;
        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);
        return 0;
    }

    gpiod_line* btn = gpiod_chip_get_line(chip, GPIO_BTN_LINE);
    if (!btn || gpiod_line_request_both_edges_events(btn, "capture-btn") < 0) {
        fprintf(stderr, "gpiod button setup failed, fallback --no-gpio\n");
        if (btn) gpiod_line_release(btn);
        gpiod_chip_close(chip);
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_running = true;
        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);
        return 0;
    }

    printf("GPIO ready (gpio72). Press button to start.\n\n");

    int session_num = 0;

    while (g_running) {
        // ── 等按钮 (空闲状态) ──
        {
            struct timespec timeout = {0, 200000000};  // 200ms
            int ret = gpiod_line_event_wait(btn, &timeout);
            if (ret <= 0) continue;

            gpiod_line_event ev;
            if (gpiod_line_event_read(btn, &ev) < 0) break;
            if (ev.event_type != GPIOD_LINE_EVENT_FALLING_EDGE) continue;
        }

        // ── 开始录制 ──
        session_num++;
        g_session_running = true;
        led_set(1);

        std::string ses_dir = make_session_dir(prefix, session_num);
        run_session(ses_dir, session_num, use_imu, use_as5600, use_vive, btn);

        led_set(0);

        if (single_shot) break;
    }

    gpiod_line_release(btn);
    gpiod_chip_close(chip);

    printf("\n=== Unified Capture Exit ===\n");
    return 0;
}
