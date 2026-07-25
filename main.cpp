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
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <gpiod.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "USBCam_API.h"
}

#include "camera_config.h"
#include "sensor.h"
#include "video_sensor.h"
#include "sixcam_sensor.h"
#include "imu_sensor.h"
#include "encoder_sensor.h"
#include "vive_tracker.h"
#include "as5600.h"

// ============================================================
// 全局状态
// ============================================================
struct timespec g_t0;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_session_running{false};
std::atomic<int> g_jhh2_remaining{0};  // jhh04 等待此计数器归零后才启流
std::atomic<bool> g_jhh02_init_done{false};  // jhh02 优先启流标志

// Socket 控制
static std::atomic<bool> g_socket_start_request{false};
static std::atomic<bool> g_ready{false};
static bool g_as5600_ok = false;
static bool g_vive_ok = false;
static bool g_use_imu = true;     // runtime --no-imu flag
static int g_sock_fd = -1;
static pthread_t g_socket_thread;
static std::string g_prefix;
static std::atomic<int> g_session_num{0};
static char g_current_session[64] = {0};

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
    // JHH04/JHH02 已移至 SixCamSensor 统一管理, 不再作为独立 VideoSensor
    // (见 sixcam_sensor.h 中的 SixCamSensor)
};

// ============================================================
// 六目模组: 一个物理主板, 两个通道 (JHH04四目 + JHH02双目)
// 由 SixCamSensor 统一管理, 避免 TSTC SDK 同 VID/PID 流的死锁
// ============================================================
struct SixCamEntry {
    bool enabled = true;
    // JHH04: 1bcf:2d51, group_order=0  (四目, 仅Y8)
    v4l2_dev_sys_data_t* jhh04_dev = nullptr;
    // JHH02: 1bcf:2d50, group_order=2  (双目, H.265+Y8)
    v4l2_dev_sys_data_t* jhh02_dev = nullptr;
};
static SixCamEntry g_sixcam;
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

    // ── 六目模组: 匹配 JHH04 (1bcf:2d51 group=0) + JHH02 (1bcf:2d50 group=2) ──
    {
        v4l2_dev_sys_data_t* six_devs = nullptr;
        int six_n = TST_USBCam_DEVICE_FIND_ID(&six_devs, SIX_VID, SIX_PID);
        if (six_n > 0) {
            g_sixcam.jhh04_dev = &six_devs[0];  // group_order=0
            g_sixcam.enabled = true;
            printf("  %-12s → [%04x:%04x group:0] %s  3104x480@30  IMU=Y (SixCam)\n",
                   "jhh04", SIX_VID, SIX_PID, g_sixcam.jhh04_dev->Device_Path);
            active++;
        } else {
            g_sixcam.enabled = false;
            fprintf(stderr, "WARN: sixcam JHH04 [%04x:%04x] not found → disabled\n",
                    SIX_VID, SIX_PID);
        }

        // JHH2 组查找 (六目 JHH02 和独立 JHH2 共用 VID/PID)
        VidPidGroup* jhh2_grp = nullptr;
        for (auto& g : groups) {
            if (g.vid == JHH2_VID && g.pid == JHH2_PID) { jhh2_grp = &g; break; }
        }
        if (!jhh2_grp) {
            VidPidGroup g;
            g.vid = JHH2_VID; g.pid = JHH2_PID;
            g.count = TST_USBCam_DEVICE_FIND_ID(&g.devs, JHH2_VID, JHH2_PID);
            groups.push_back(g);
            jhh2_grp = &groups.back();
        }

        // ── 六目模组 JHH02 + JHH2 独立双目: 按 Product 字符串区分 ──
        //     TSTC SDK 字段: iProduct 区分设备类型
        //       独立 JHH2 双目: iProduct 含 "CYBER"
        //       六目模块 JHH02:  iProduct 不含 "CYBER"
        if (g_sixcam.enabled) {
            v4l2_dev_sys_data_t* jhh02_found = nullptr;
            v4l2_dev_sys_data_t* cyber_devs[2] = {nullptr, nullptr};
            int cyber_count = 0;

            for (int i = 0; i < jhh2_grp->count; i++) {
                const char* prod = jhh2_grp->devs[i].iProduct;
                if (prod && strstr(prod, "CYBER")) {
                    if (cyber_count < 2) cyber_devs[cyber_count++] = &jhh2_grp->devs[i];
                } else if (!jhh02_found) {
                    jhh02_found = &jhh2_grp->devs[i];
                }
            }

            // ── 1. 分配 jhh02（六目模块的 JHH2 接口）──
            if (jhh02_found) {
                g_sixcam.jhh02_dev = jhh02_found;
                printf("  %-12s → [%04x:%04x] %s  3104x480@30  IMU=Y (SixCam, prod='%s')\n",
                       "jhh02", JHH2_VID, JHH2_PID,
                       jhh02_found->Device_Path, jhh02_found->iProduct);
            } else {
                fprintf(stderr, "WARN: sixcam JHH02 [%04x:%04x] not found by product\n",
                        JHH2_VID, JHH2_PID);
            }

            // ── 2. 重新分配独立 JHH2 双目 (按 CYBER product) ──
            //     清除旧 group_order 分配, 按实际 CYBER 数量重分
            for (int j = 0; j < N_CAMS; j++) {
                if (CAMS[j].cfg.vid == JHH2_VID && CAMS[j].cfg.pid == JHH2_PID) {
                    if (CAMS[j].dev_ptr != jhh02_found) {
                        CAMS[j].enabled = false;
                        CAMS[j].dev_ptr = nullptr;
                    }
                }
            }
            if (cyber_count >= 1) {
                CAMS[0].enabled = true;
                CAMS[0].dev_ptr = cyber_devs[0];
                printf("  %-12s → [%04x:%04x] %s  %dx%d@%d  IMU=Y (CYBER #1)\n",
                       CAMS[0].cfg.name, JHH2_VID, JHH2_PID,
                       cyber_devs[0]->Device_Path,
                       CAMS[0].cfg.width, CAMS[0].cfg.height, CAMS[0].cfg.fps);
            }
            if (cyber_count >= 2) {
                CAMS[1].enabled = true;
                CAMS[1].dev_ptr = cyber_devs[1];
                printf("  %-12s → [%04x:%04x] %s  %dx%d@%d  IMU=Y (CYBER #2)\n",
                       CAMS[1].cfg.name, JHH2_VID, JHH2_PID,
                       cyber_devs[1]->Device_Path,
                       CAMS[1].cfg.width, CAMS[1].cfg.height, CAMS[1].cfg.fps);
            }
            if (cyber_count == 0 && !jhh02_found) {
                fprintf(stderr, "WARN: no JHH2 devices found (cyber=%d)\n", cyber_count);
            }
        }
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
    // 六目模组目录
    if (g_sixcam.enabled) {
        snprintf(buf, sizeof(buf), "%s/jhh04", dir.c_str());
        mkdir_p(buf, 0755);
        snprintf(buf, sizeof(buf), "%s/jhh02", dir.c_str());
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
// 外设探测 (启动时运行一次, 结果供 status 命令使用)
// ============================================================
static void probe_peripherals(bool probe_as5600, bool probe_vive) {
    // ── AS5600 探测 ──
    if (probe_as5600) {
        as5600_dev_t dev = as5600_open(ENC_I2C_PATH, ENC_I2C_ADDR);
        if (dev) {
            if (as5600_probe(dev) == 0) {
                g_as5600_ok = true;
                printf("[probe] AS5600 detected at %s (0x%02x)\n", ENC_I2C_PATH, ENC_I2C_ADDR);
            } else {
                printf("[probe] AS5600 probe failed at %s\n", ENC_I2C_PATH);
            }
            as5600_close(dev);
        } else {
            printf("[probe] AS5600: cannot open %s\n", ENC_I2C_PATH);
        }
    }

    // ── VIVE USB 探测 ──
    if (probe_vive) {
        DIR* dir = opendir("/sys/bus/usb/devices");
        if (dir) {
            struct dirent* entry;
            char path[512], buf[64];
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", entry->d_name);
                int fd = open(path, O_RDONLY);
                if (fd < 0) continue;
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) continue;
                if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
                else buf[n] = '\0';
                if (strcmp(buf, "28de") != 0) continue;

                snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", entry->d_name);
                fd = open(path, O_RDONLY);
                if (fd < 0) continue;
                n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) continue;
                if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
                else buf[n] = '\0';
                if (strcmp(buf, "2300") != 0) continue;

                g_vive_ok = true;
                printf("[probe] VIVE Tracker (28de:2300) found at %s\n", entry->d_name);
                break;
            }
            closedir(dir);
        }
        if (!g_vive_ok)
            printf("[probe] VIVE Tracker not found\n");
    }
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

    // ★ 统计所有 JHH2 设备数量, jhh04 必须等它们全部启流完毕
    g_jhh2_remaining = 0;
    for (int i = 0; i < N_CAMS; i++) {
        if (CAMS[i].enabled && CAMS[i].cfg.vid == JHH2_VID && CAMS[i].cfg.pid == JHH2_PID)
            g_jhh2_remaining++;
    }
    if (g_sixcam.enabled && g_sixcam.jhh02_dev)
        g_jhh2_remaining++;  // 六目的 jhh02
    fprintf(stderr, "[main] JHH2 devices to init before jhh04: %d\n", (int)g_jhh2_remaining);

    // ★ jhh02 优先策略: 独立 JHH2 等 jhh02 先完成 STREAM_STATUS
    g_jhh02_init_done = !(g_sixcam.enabled && g_sixcam.jhh02_dev);

    // ---- 创建 VideoSensor + ImuSensor (独立 JHH2) ----
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

    // ---- 六目模组 (SixCamSensor: JHH04 + JHH02) ----
    if (g_sixcam.enabled && g_sixcam.jhh04_dev && g_sixcam.jhh02_dev) {
        // 定义六目模组两个通道的配置
        CameraConfig jhh04_cfg{"jhh04", SIX_VID,  SIX_PID,  0, 3104, 480, 30, 4000000, 30,
                                true, ImuOrientation::HORIZONTAL_TOP, false, true};
        CameraConfig jhh02_cfg{"jhh02", JHH2_VID, JHH2_PID, 2, 3104, 480, 30, 4000000, 30,
                                true, ImuOrientation::HORIZONTAL_TOP, true, true};

        auto* sc = new SixCamSensor(jhh04_cfg, jhh02_cfg,
                                     *g_sixcam.jhh04_dev, *g_sixcam.jhh02_dev,
                                     ses_dir, session_num, g_session_running);
        sensors.push_back(sc);

        // 六目模组的两个 IMU 传感器
        if (use_imu) {
            sensors.push_back(new ImuSensor("jhh04", ses_dir,
                sc->imu_queue_jhh04(), session_num,
                ImuOrientation::HORIZONTAL_TOP, g_session_running));
            sensors.push_back(new ImuSensor("jhh02", ses_dir,
                sc->imu_queue_jhh02(), session_num,
                ImuOrientation::HORIZONTAL_TOP, g_session_running));
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
// Socket 控制 — 命令处理
// ============================================================
static const char* SOCK_PATH = "/tmp/unified_capture.sock";

// 构建 cameras JSON 子对象
static std::string cameras_json() {
    std::string s = "\"cameras\":{";
    bool first = true;
    for (int i = 0; i < N_CAMS; i++) {
        if (!first) s += ",";
        s += "\"" + std::string(CAMS[i].cfg.name) + "\":" + (CAMS[i].enabled ? "true" : "false");
        first = false;
    }
    if (g_sixcam.enabled) {
        if (!first) s += ",";
        s += "\"jhh04\":" + std::string(g_sixcam.jhh04_dev ? "true" : "false");
        s += ",\"jhh02\":" + std::string(g_sixcam.jhh02_dev ? "true" : "false");
    }
    s += "}";
    return s;
}

static std::string handle_start() {
    if (!g_ready) {
        return "{\"ok\":false,\"error\":\"not ready\"}";
    }
    if (g_session_running) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"ok\":false,\"error\":\"already running\",\"session\":\"%s\"}",
            g_current_session);
        return buf;
    }

    g_socket_start_request = true;

    // 自旋等待主线程启动 session (最多 10s)
    for (int i = 0; i < 1000 && !g_session_running; i++) {
        usleep(10000);  // 10ms
    }

    if (!g_session_running) {
        g_socket_start_request = false;
        return "{\"ok\":false,\"error\":\"start timeout\"}";
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"session\":\"%s\"}", g_current_session);
    return buf;
}

static std::string handle_stop() {
    if (!g_session_running) {
        return "{\"ok\":false,\"error\":\"not running\"}";
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - g_t0.tv_sec) * 1000
                    + (now.tv_nsec - g_t0.tv_nsec) / 1000000;

    g_session_running = false;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"elapsed_ms\":%ld}", elapsed_ms);
    return buf;
}

static std::string handle_status() {
    if (!g_ready) {
        return "{\"ok\":true,\"ready\":false,\"running\":false,"
               "\"session\":null,\"elapsed_ms\":0,"
               "\"cameras\":{},\"imu\":false,\"as5600\":false,\"vive\":false}";
    }

    const char* session_str = "null";
    long elapsed_ms = 0;
    if (g_session_running) {
        session_str = g_current_session;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec - g_t0.tv_sec) * 1000
                    + (now.tv_nsec - g_t0.tv_nsec) / 1000000;
    }

    char buf[1024];
    if (g_session_running) {
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"ready\":true,\"running\":true,"
            "\"session\":\"%s\",\"elapsed_ms\":%ld,%s,"
            "\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
            session_str, elapsed_ms, cameras_json().c_str(),
            g_use_imu ? "true" : "false",
            g_as5600_ok ? "true" : "false",
            g_vive_ok ? "true" : "false");
    } else {
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"ready\":true,\"running\":false,"
            "\"session\":null,\"elapsed_ms\":0,%s,"
            "\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
            cameras_json().c_str(),
            g_use_imu ? "true" : "false",
            g_as5600_ok ? "true" : "false",
            g_vive_ok ? "true" : "false");
    }
    return buf;
}

// ============================================================
// Socket 线程
// ============================================================
static void* socket_thread(void*) {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "[socket] socket() failed: %s\n", strerror(errno));
        return nullptr;
    }
    g_sock_fd = listen_fd;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    // 清理残留 sock 文件: 先 connect 试探, 失败则 unlink
    int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (test_fd >= 0) {
        if (connect(test_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            unlink(SOCK_PATH);
        }
        close(test_fd);
    }

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[socket] bind(%s) failed: %s\n", SOCK_PATH, strerror(errno));
        close(listen_fd);
        g_sock_fd = -1;
        return nullptr;
    }

    if (listen(listen_fd, 4) < 0) {
        fprintf(stderr, "[socket] listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        g_sock_fd = -1;
        return nullptr;
    }

    printf("[socket] listening on %s\n", SOCK_PATH);

    while (g_running) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        char buf[256];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            // 去掉末尾换行
            if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';

            std::string response;
            if (strcmp(buf, "start") == 0)          response = handle_start();
            else if (strcmp(buf, "stop") == 0)      response = handle_stop();
            else if (strcmp(buf, "status") == 0)    response = handle_status();
            else response = "{\"ok\":false,\"error\":\"unknown command\"}";

            response += "\n";
            ssize_t ignored = write(client_fd, response.c_str(), response.size());
            (void)ignored;
        }
        close(client_fd);
    }

    close(listen_fd);
    return nullptr;
}

// ============================================================
static void socket_init() {
    pthread_create(&g_socket_thread, nullptr, socket_thread, nullptr);
}

static void socket_cleanup() {
    if (g_sock_fd >= 0) {
        shutdown(g_sock_fd, SHUT_RDWR);
    }
    pthread_join(g_socket_thread, nullptr);
    unlink(SOCK_PATH);
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
    g_prefix = prefix;
    g_use_imu = use_imu;

    // ---- Socket 先初始化 (设备扫描可能耗时, 前端可先连上等 ready) ----
    socket_init();

    // ---- 按 VID/PID 自动匹配摄像头设备 ----
    int active = resolve_camera_devices();
    if (active <= 0) {
        fprintf(stderr, "ERROR: No cameras found via TSTC SDK\n");
    g_running = false;
    socket_cleanup();
        return 1;
    }

    // ---- 外设探测 ----
    probe_peripherals(use_as5600, use_vive);
    g_ready = true;

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
        g_session_num = 1;
        snprintf(g_current_session, sizeof(g_current_session), "session_%03d", 1);
        clock_gettime(CLOCK_MONOTONIC, &g_t0);  // 先写时间基, 再设 running
        g_session_running = true;

        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);

        printf("=== Unified Capture Exit ===\n");
    g_running = false;
    socket_cleanup();
        return 0;
    }

    // ---- GPIO 模式: 按钮切换开始/停止, Ctrl-C 退出 ----
    gpiod_chip* chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        fprintf(stderr, "gpiod_chip_open(%s) failed, fallback --no-gpio\n", GPIO_CHIP);
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_num = 1;
        snprintf(g_current_session, sizeof(g_current_session), "session_%03d", 1);
        clock_gettime(CLOCK_MONOTONIC, &g_t0);
        g_session_running = true;
        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);
    g_running = false;
    socket_cleanup();
        return 0;
    }

    gpiod_line* btn = gpiod_chip_get_line(chip, GPIO_BTN_LINE);
    if (!btn || gpiod_line_request_both_edges_events(btn, "capture-btn") < 0) {
        fprintf(stderr, "gpiod button setup failed, fallback --no-gpio\n");
        if (btn) gpiod_line_release(btn);
        gpiod_chip_close(chip);
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_num = 1;
        snprintf(g_current_session, sizeof(g_current_session), "session_%03d", 1);
        clock_gettime(CLOCK_MONOTONIC, &g_t0);
        g_session_running = true;
        std::string ses_dir = make_session_dir(prefix, 1);
        run_session(ses_dir, 1, use_imu, use_as5600, use_vive, nullptr);
    g_running = false;
    socket_cleanup();
        return 0;
    }

    printf("GPIO ready (gpio72). Press button to start.\n\n");

    int session_num = 0;

    while (g_running) {
        // ── 等启动信号 (GPIO 按钮 或 Socket start) ──
        bool do_start = false;
        {
            struct timespec timeout = {0, 200000000};  // 200ms
            int ret = gpiod_line_event_wait(btn, &timeout);
            if (ret > 0) {
                gpiod_line_event ev;
                if (gpiod_line_event_read(btn, &ev) < 0) break;
                if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
                    do_start = true;
            }
        }
        // Socket 触发 start
        if (g_socket_start_request.exchange(false))
            do_start = true;

        if (!do_start) continue;

        // ── 开始录制 ──
        session_num++;
        g_session_num = session_num;
        snprintf(g_current_session, sizeof(g_current_session),
                 "session_%03d", session_num);
        clock_gettime(CLOCK_MONOTONIC, &g_t0);  // 先写时间基和 session 名, 再设 running
        g_session_running = true;
        led_set(1);

        std::string ses_dir = make_session_dir(prefix, session_num);
        run_session(ses_dir, session_num, use_imu, use_as5600, use_vive, btn);

        led_set(0);

        if (single_shot) break;
    }

    gpiod_line_release(btn);
    gpiod_chip_close(chip);

    socket_cleanup();
    printf("\n=== Unified Capture Exit ===\n");
    return 0;
}
