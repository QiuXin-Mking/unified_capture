#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <gpiod.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "Nori_Xvision_API.h"
}

#include "camera_config.h"
#include "barrier.h"
#include "hardware/common/sensor.h"
#include "hardware/VideoSensor/VideoSensor.h"
#include "hardware/VideoSensor/SixCamSensor.h"
#include "hardware/IMU/ImuSensor.h"
#include "hardware/as5600/encoder_sensor.h"
#include "output_path.h"
#include "vive_usb.h"
#include "hardware/tracker/ViveTrackerSensor.h"

// ============================================================
// 全局状态
// ============================================================
struct timespec g_t0;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_session_running{false};
std::atomic<int> g_jhh2_remaining{0};
std::atomic<bool> g_jhh02_init_done{false};

// Socket (单线程, 在主循环中轮询)
static const char* SOCK_PATH = "/tmp/unified_capture.sock";
static int g_sock_fd = -1;
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_socket_start_request{false};
static bool g_use_h265 = true;
static bool g_use_vive = true;
static bool g_use_imu = true;
static bool g_use_as5600 = false;

// Preview JPEG export (no extra thread — flags are polled in sensor collect loops)
std::atomic<bool> g_preview_pending{false};
std::string g_preview_path;
std::mutex g_preview_mutex;

static void sig_handler(int sig) {
    if (sig == SIGSEGV || sig == SIGABRT) {
        fprintf(stderr, "\n!!! FATAL: caught signal %d\n", sig);
        fflush(stderr);
        _exit(1);
    }
    g_running = false;
    g_session_running = false;
}

// ============================================================
// 摄像头配置
// ============================================================
#define JHH2_VID  0x1bcf
#define JHH2_PID  0x2d50
#define SIX_VID   0x1bcf
#define SIX_PID   0x2d51

struct CamEntry {
    CameraConfig cfg;
    bool enabled = true;
};

static CamEntry CAMS[] = {
{{"jhh2_left",  JHH2_VID, JHH2_PID, 0, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true, -1},  true},
{{"jhh2_right", JHH2_VID, JHH2_PID, 1, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true, -1},  true},
};

struct SixCamEntry {
    bool enabled = true;
    uint32_t jhh04_id = 0;
    uint32_t jhh02_id = 0;
};
static SixCamEntry g_sixcam;
static const int N_CAMS = sizeof(CAMS) / sizeof(CAMS[0]);

static const char*  ENC_I2C_PATH    = "/dev/i2c-6";
static const int    ENC_I2C_ADDR    = 0x36;
static const int    ENC_INTERVAL_US = 10000;

static const char*  GPIO_CHIP     = "/dev/gpiochip2";
static const int    GPIO_BTN_LINE = 8;
#define LED_PATH      "/sys/class/leds/sys_led"

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
static void scan_devices() {
    uint32_t count = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &count);
    if (ret != NORI_OK) {
        printf("Nori_Xvision_Init failed: 0x%x\n", ret);
        return;
    }
    printf("Found %u device(s):\n", count);
    for (uint32_t i = 0; i < count; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);
        printf("  [%u] %04x:%04x \"%s\" \"%s\" %s\n",
               i, info.idVendor, info.idProduct,
               info.iManufacturer, info.iProduct, info.device);
        VERSION_INFO ver;
        if (Nori_Xvision_GetVersion(i, &ver) == NORI_OK) {
            printf("       SDK:%s  Type:%s\n", ver.SDKVersion, ver.DeviceType);
        }
    }
    Nori_Xvision_UnInit();
}

struct VidPidGroup { uint16_t vid, pid; std::vector<uint32_t> device_ids; };

static int resolve_camera_devices() {
    // ★ 新 SDK: 全局 Init 枚举所有设备
    uint32_t total_devices = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &total_devices);
    if (ret != NORI_OK) {
        fprintf(stderr, "ERROR: Nori_Xvision_Init failed: 0x%x\n", ret);
        return 0;
    }
    printf("Nori Xvision SDK: found %u device(s)\n", total_devices);

    if (total_devices == 0) {
        Nori_Xvision_UnInit();
        return 0;
    }

    // 按 VID/PID 分组所有设备
    std::vector<VidPidGroup> groups;
    for (uint32_t i = 0; i < total_devices; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);

        VidPidGroup* grp = nullptr;
        for (auto& g : groups) {
            if (g.vid == info.idVendor && g.pid == info.idProduct) { grp = &g; break; }
        }
        if (!grp) {
            VidPidGroup g;
            g.vid = info.idVendor;
            g.pid = info.idProduct;
            groups.push_back(g);
            grp = &groups.back();
        }
        grp->device_ids.push_back(i);

        printf("  Device[%u]: %04x:%04x \"%s\" \"%s\" %s\n",
               i, info.idVendor, info.idProduct,
               info.iManufacturer, info.iProduct, info.device);
    }

    // 匹配 JHH2 独立相机 (1bcf:2d50, 取前 2 个)
    VidPidGroup* jhh2_grp = nullptr;
    for (auto& g : groups) {
        if (g.vid == JHH2_VID && g.pid == JHH2_PID) { jhh2_grp = &g; break; }
    }
    if (jhh2_grp) {
        for (int i = 0; i < N_CAMS && i < (int)jhh2_grp->device_ids.size(); i++) {
            auto& cam = CAMS[i];
            if (!cam.enabled) continue;
            cam.cfg.device_id = jhh2_grp->device_ids[i];
            DEVICE_INFO info;
            Nori_Xvision_GetDeviceInfo(cam.cfg.device_id, &info);
            printf("  %-12s -> device[%u] %s  %dx%d@%d IMU=%c\n",
                   cam.cfg.name, cam.cfg.device_id, info.device,
                   cam.cfg.width, cam.cfg.height, cam.cfg.fps,
                   cam.cfg.has_imu ? 'Y' : 'N');
        }
    }

    int active = 0;
    for (int i = 0; i < N_CAMS; i++) if (CAMS[i].enabled && CAMS[i].cfg.device_id >= 0) active++;

    // 匹配六目模组 jhh04 (1bcf:2d51)
    for (auto& g : groups) {
        if (g.vid == SIX_VID && g.pid == SIX_PID && !g.device_ids.empty()) {
            g_sixcam.jhh04_id = g.device_ids[0];
            g_sixcam.enabled = true;
            DEVICE_INFO info;
            Nori_Xvision_GetDeviceInfo(g_sixcam.jhh04_id, &info);
            printf("  %-12s -> device[%u] %s  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh04", g_sixcam.jhh04_id, info.device);
            active++;
            break;
        }
    }

    // jhh02 (六目双目侧): 从 jhh2_grp 中取 group_order=2
    if (g_sixcam.enabled && jhh2_grp && jhh2_grp->device_ids.size() >= 3) {
        g_sixcam.jhh02_id = jhh2_grp->device_ids[2];
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(g_sixcam.jhh02_id, &info);
        printf("  %-12s -> device[%u] %s  4000x1200@30 IMU=Y (SixCam)\n",
               "jhh02", g_sixcam.jhh02_id, info.device);
    } else if (g_sixcam.enabled) {
        fprintf(stderr, "WARN: jhh02 not found (need 3+ 1bcf:2d50 devices)\n");
        g_sixcam.enabled = false;
        active--;
    }

    return active;
}

// ============================================================
static std::string make_session_dir(const std::string& prefix, int num) {
    char buf[256]; snprintf(buf, sizeof(buf), "%s/session_%03d", prefix.c_str(), num);
    std::string dir(buf); mkdir_p(buf, 0755);
    for (int i = 0; i < N_CAMS; i++) {
        if (!CAMS[i].enabled) continue;
        snprintf(buf, sizeof(buf), "%s/%s", dir.c_str(), CAMS[i].cfg.name); mkdir_p(buf, 0755);
    }
    if (g_sixcam.enabled) {
        snprintf(buf, sizeof(buf), "%s/jhh04", dir.c_str()); mkdir_p(buf, 0755);
        snprintf(buf, sizeof(buf), "%s/jhh02", dir.c_str()); mkdir_p(buf, 0755);
    }
    return dir;
}

// ============================================================
// Socket 控制 (单线程, 无额外 pthread)
// ============================================================
static std::string cameras_json() {
    std::string s = "\"cameras\":{"; bool first = true;
    for (int i = 0; i < N_CAMS; i++) {
        if (!first) s += ",";
        s += "\"" + std::string(CAMS[i].cfg.name) + "\":" + (CAMS[i].enabled ? "true" : "false");
        first = false;
    }
    if (g_sixcam.enabled) {
        s += ",\"jhh04\":" + std::string(g_sixcam.jhh04_id > 0 ? "true" : "false");
        s += ",\"jhh02\":" + std::string(g_sixcam.jhh02_id > 0 ? "true" : "false");
    }
    s += "}"; return s;
}

static void socket_handle_client(int fd) {
    char buf[256]; ssize_t n = read(fd, buf, sizeof(buf)-1);
    if (n <= 0) return;
    buf[n] = '\0'; if (buf[n-1]=='\n') buf[n-1]='\0';
    std::string resp;
    if (!strcmp(buf, "start")) {
        if (!g_ready) resp = "{\"ok\":false,\"error\":\"not ready\"}";
        else if (g_session_running) resp = "{\"ok\":false,\"error\":\"already running\"}";
        else { g_socket_start_request = true; resp = "{\"ok\":true}"; }
    } else if (!strcmp(buf, "stop")) {
        if (!g_session_running) resp = "{\"ok\":false,\"error\":\"not running\"}";
        else { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long e = (now.tv_sec-g_t0.tv_sec)*1000 + (now.tv_nsec-g_t0.tv_nsec)/1000000;
            g_session_running = false;
            char b[64]; snprintf(b,sizeof(b),"{\"ok\":true,\"elapsed_ms\":%ld}",e); resp = b; }
    } else if (!strncmp(buf, "preview:", 8)) {
        const char* path = buf + 8;
        if (!g_session_running) {
            resp = "{\"ok\":false,\"error\":\"not running\"}";
        } else {
            std::lock_guard<std::mutex> lock(g_preview_mutex);
            g_preview_path = path;
            g_preview_pending = true;
            resp = "{\"ok\":true}";
        }
    } else if (!strcmp(buf, "status")) {
        if (!g_ready) {
            char buf[512]; snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"ready\":false,\"running\":false,\"session\":null,\"elapsed_ms\":0,\"cameras\":{},\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
                g_use_imu?"true":"false", g_use_as5600?"true":"false", g_use_vive?"true":"false");
            resp = buf;
        }
        else {
            long el = 0;
            if (g_session_running) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                el = (now.tv_sec-g_t0.tv_sec)*1000 + (now.tv_nsec-g_t0.tv_nsec)/1000000; }
            char b[1024]; snprintf(b,sizeof(b),
                "{\"ok\":true,\"ready\":true,\"running\":%s,\"session\":null,\"elapsed_ms\":%ld,%s,\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
                g_session_running?"true":"false", el, cameras_json().c_str(),
                g_use_imu?"true":"false", g_use_as5600?"true":"false", g_use_vive?"true":"false");
            resp = b;
        }
    } else {
        resp = "{\"ok\":false,\"error\":\"unknown command\"}";
    }
    resp += "\n"; write(fd, resp.c_str(), resp.size());
}

static int socket_setup() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    int t = socket(AF_UNIX, SOCK_STREAM, 0);
    if (t >= 0) { if (connect(t,(struct sockaddr*)&addr,sizeof(addr))<0) unlink(SOCK_PATH); close(t); }
    if (bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("bind"); close(fd); return -1; }
    if (listen(fd,4)<0) { perror("listen"); close(fd); return -1; }
    // 设为非阻塞, 主循环用 poll 检测
    int flags = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, flags|O_NONBLOCK);
    printf("[socket] listening on %s (poll mode)\n", SOCK_PATH);
    return fd;
}

// ============================================================
static void run_session(const std::string& ses_dir, int session_num,
                        bool use_imu, bool use_as5600, bool use_vive,
                        gpiod_line* btn = nullptr) {
    printf("\n>>> Session %d START <<<\n", session_num);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);

    // 生成时间戳字符串: YYYYMMDD-HH_MM_SS
    time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
    char ts_buf[64]; snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d_%02d_%02d",
        tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string session_ts = ts_buf;

    std::vector<Sensor*> sensors;

    g_jhh2_remaining = 0;
    for (int i = 0; i < N_CAMS; i++)
        if (CAMS[i].enabled && CAMS[i].cfg.vid==JHH2_VID && CAMS[i].cfg.pid==JHH2_PID) g_jhh2_remaining++;
    if (g_sixcam.enabled && g_sixcam.jhh02_id) g_jhh2_remaining++;

    for (int i = 0; i < N_CAMS; i++) {
        if (!CAMS[i].enabled) continue;
        auto& cam = CAMS[i];
        auto* vs = new VideoSensor(cam.cfg, ses_dir, (uint32_t)cam.cfg.device_id, session_num, session_ts, g_session_running);
        sensors.push_back(vs);
        if (use_imu && cam.cfg.has_imu)
            sensors.push_back(new ImuSensor(cam.cfg.name, ses_dir, vs->imu_queue(), session_num, session_ts, cam.cfg.imu_orientation, g_session_running));
    }
    if (g_sixcam.enabled && g_sixcam.jhh04_id > 0 && g_sixcam.jhh02_id > 0) {
        CameraConfig j04{"jhh04",SIX_VID,SIX_PID,0,3104,480,30,4000000,30,true,ImuOrientation::HORIZONTAL_TOP,false,true,-1};
        CameraConfig j02{"jhh02",JHH2_VID,JHH2_PID,2,4000,1200,30,16000000,30,true,ImuOrientation::HORIZONTAL_TOP,g_use_h265,true,-1};
        auto* sc = new SixCamSensor(j04,j02,g_sixcam.jhh04_id,g_sixcam.jhh02_id,ses_dir,session_num,session_ts,g_session_running);
        sensors.push_back(sc);
        if (use_imu) {
            sensors.push_back(new ImuSensor("jhh04",ses_dir,sc->imu_queue_jhh04(),session_num,session_ts,ImuOrientation::HORIZONTAL_TOP,g_session_running));
            sensors.push_back(new ImuSensor("jhh02",ses_dir,sc->imu_queue_jhh02(),session_num,session_ts,ImuOrientation::HORIZONTAL_TOP,g_session_running));
        }
    }
    if (use_as5600) sensors.push_back(new EncoderSensor(ENC_I2C_PATH,ENC_I2C_ADDR,ses_dir,session_num,session_ts,ENC_INTERVAL_US,g_session_running));
    if (use_vive) sensors.push_back(new ViveTrackerSensor(ses_dir,session_num,session_ts,g_session_running));

    if (sensors.empty()) { fprintf(stderr,"WARN: no sensors\n"); return; }

    // ★ Barrier 只包含 sensor 线程, main 线程不入
    //    这样 main 可以在等 sensor setup 的同时处理 socket 命令
    SimpleBarrier gate(sensors.size());
    for (auto* s : sensors) s->launch(gate);

    while (!gate.wait_all_arrived(100)) {
        if (g_sock_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = g_sock_fd; pfd.events = POLLIN;
            if (poll(&pfd, 1, 0) > 0) {
                int c = accept(g_sock_fd, nullptr, nullptr);
                if (c >= 0) { socket_handle_client(c); close(c); }
            }
        }
    }
    printf(">>> ALL SENSORS GO <<<\n");

    // 等停止信号: GPIO + Socket
    while (g_session_running) {
        struct pollfd pfds[2];
        int np = 0;
        if (btn) { pfds[np].fd = gpiod_line_event_get_fd(btn); pfds[np].events = POLLIN; np++; }
        if (g_sock_fd >= 0) { pfds[np].fd = g_sock_fd; pfds[np].events = POLLIN; np++; }
        int ret = poll(pfds, np, 50);
        if (ret > 0) {
            for (int i = 0; i < np; i++) {
                if (pfds[i].revents & POLLIN) {
                    if (btn && pfds[i].fd == gpiod_line_event_get_fd(btn)) {
                        gpiod_line_event ev; gpiod_line_event_read(btn, &ev);
                        if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) g_session_running = false;
                    } else if (g_sock_fd >= 0 && pfds[i].fd == g_sock_fd) {
                        int c = accept(g_sock_fd, nullptr, nullptr);
                        if (c >= 0) { socket_handle_client(c); close(c); }
                    }
                }
            }
        }
    }
    printf("\n>>> Session %d STOP <<<\n", session_num);
    for (auto* s : sensors) { s->join(); delete s; }
    printf(">>> Session %d DONE <<<\n\n", session_num);
}

// ============================================================
static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS] [output_prefix]\n", prog);
}

// ============================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, sig_handler);
    signal(SIGABRT, sig_handler);
    setlinebuf(stdout);
    setlinebuf(stderr);

    bool use_gpio=true, use_socket=false, single_shot=false;
    g_use_as5600 = true; g_use_imu = true; g_use_vive = true;
    std::string prefix;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--scan")) { scan_devices(); return 0; }
        else if (!strcmp(argv[i],"--no-gpio")) use_gpio=false;
        else if (!strcmp(argv[i],"--socket")) { use_socket=true; use_gpio=false; }
        else if (!strcmp(argv[i],"--no-as5600")) g_use_as5600=false;
        else if (!strcmp(argv[i],"--no-imu")) g_use_imu=false;
        else if (!strcmp(argv[i],"--no-h265")) g_use_h265=false;
        else if (!strcmp(argv[i],"--single")) single_shot=true;
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) { print_usage(argv[0]); return 0; }
        else if (argv[i][0]!='-') prefix=argv[i];
    }
    prefix = capture_output_prefix(prefix);
    if (!is_sd_capture_path(prefix)) {
        fprintf(stderr, "ERROR: output must be under %s: %s\n", kSdCaptureRoot, prefix.c_str());
        return 2;
    }
    if (!is_sd_card_mounted()) {
        fprintf(stderr, "ERROR: SD card is not mounted at %s\n", kSdMountPath);
        return 2;
    }
    if (mkdir_p(kSdCaptureRoot, 0755) != 0) {
        perror(kSdCaptureRoot);
        return 2;
    }

    // --no-h265: 全局禁用 H.265 编码 (排查用)
    if (!g_use_h265) {
        for (int i = 0; i < N_CAMS; i++) CAMS[i].cfg.output_h265 = false;
        printf("[note] H.265 disabled (--no-h265), output Y8 only\n");
    }

    int active = resolve_camera_devices();
    if (active <= 0) { fprintf(stderr,"ERROR: No cameras\n"); return 1; }

    // ★ VIVE Tracker 自动检测: 不依赖 --no-vive 开关
    int vive_count = detect_vive_trackers();
    if (vive_count <= 0) {
        g_use_vive = false;
        printf("[vive] no tracker detected, VIVE disabled\n");
    } else {
        g_use_vive = true;
        printf("[vive] %d tracker(s) detected, VIVE enabled\n", vive_count);
    }

    g_ready = true;

    // ★ Socket 在主线程建立 (无额外线程, 避免干扰 TSTC/MPP)
    g_sock_fd = socket_setup();

    printf("\n=== Unified Capture (%d camera(s)) ===\n", active);
    led_disable_trigger(); led_set(0);

    // --socket 模式: 纯 socket 控制, --single 每次退出让 systemd 重启
    if (use_socket) {
        printf("Socket mode. Use 'echo start|nc -U %s' to record.\n\n", SOCK_PATH);
        // 从已有目录推断起始 session 号
        int session_num = 0;
        for (int s = 1; s < 999; s++) {
            char tmp[256]; snprintf(tmp, sizeof(tmp), "%s/session_%03d", prefix.c_str(), s);
            struct stat st; if (stat(tmp, &st) == 0) session_num = s;
        }
        printf("Starting from session_%03d\n", session_num + 1);
        while (g_running) {
            struct pollfd pfd;
            pfd.fd = g_sock_fd; pfd.events = POLLIN;
            while (g_running && poll(&pfd, 1, 200) <= 0) {}
            if (!g_running) break;
            int c = accept(g_sock_fd, nullptr, nullptr);
            if (c >= 0) { socket_handle_client(c); close(c); }
            if (!g_socket_start_request.exchange(false)) continue;

            session_num++;
            g_session_running = true;
            led_set(1);
            std::string sd = make_session_dir(prefix, session_num);
            run_session(sd, session_num, g_use_imu, g_use_as5600, g_use_vive, nullptr);
            led_set(0);
            if (single_shot) break;  // --single: 跑一次就退出, 让 systemd 重启
        }
        if (g_sock_fd>=0) { close(g_sock_fd); unlink(SOCK_PATH); }
        Nori_Xvision_UnInit();
        return 0;
    }

    // --no-gpio 模式: 启动即录
    if (!use_gpio) {
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_running = true;
        std::string sd = make_session_dir(prefix, 1);
        run_session(sd, 1, g_use_imu, g_use_as5600, g_use_vive, nullptr);
        if (g_sock_fd>=0) { close(g_sock_fd); unlink(SOCK_PATH); }
        Nori_Xvision_UnInit();
        return 0;
    }

    // GPIO 模式
    gpiod_chip* chip = gpiod_chip_open(GPIO_CHIP);
    gpiod_line* btn = nullptr;
    if (chip) {
        btn = gpiod_chip_get_line(chip, GPIO_BTN_LINE);
        if (!btn || gpiod_line_request_both_edges_events(btn,"capture-btn")<0) {
            if (btn) gpiod_line_release(btn);
            gpiod_chip_close(chip);
            btn = nullptr;
            chip = nullptr;
        }
    }
    if (!btn) {
        fprintf(stderr,"GPIO unavailable; use socket start or Ctrl-C\n");
        g_session_running = true;
        std::string sd = make_session_dir(prefix, 1);
        run_session(sd,1,g_use_imu,g_use_as5600,g_use_vive,nullptr);
        if (g_sock_fd>=0) { close(g_sock_fd); unlink(SOCK_PATH); }
        printf("\n=== Exit ===\n");
        return 0;
    }

    printf("GPIO ready. Press button or use socket.\n\n");
    int session_num = 0;

    // 主循环: poll GPIO + Socket
    while (g_running) {
        struct pollfd pfds[2];
        pfds[0].fd = gpiod_line_event_get_fd(btn); pfds[0].events = POLLIN;
        pfds[1].fd = g_sock_fd; pfds[1].events = POLLIN;
        int ret = poll(pfds, 2, 200);
        if (ret < 0 && errno != EINTR) break;

        // 处理 socket (非阻塞)
        if (ret > 0 && pfds[1].revents & POLLIN) {
            int c = accept(g_sock_fd, nullptr, nullptr);
            if (c >= 0) { socket_handle_client(c); close(c); }
        }

        // ★ 检查 socket start 请求 (无论 poll 返回什么)
        bool do_start = false;
        if (g_socket_start_request.exchange(false))
            do_start = true;

        // 处理 GPIO
        if (!do_start && ret > 0 && pfds[0].revents & POLLIN) {
            gpiod_line_event ev;
            if (gpiod_line_event_read(btn, &ev) < 0) break;
            if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE)
                do_start = true;
        }

        if (!do_start) continue;

        session_num++;
        g_session_running = true;
        led_set(1);
        std::string sd = make_session_dir(prefix, session_num);
        run_session(sd, session_num, g_use_imu, g_use_as5600, g_use_vive, btn);
        led_set(0);
        if (single_shot) break;
    }

    if (btn) gpiod_line_release(btn);
    if (chip) gpiod_chip_close(chip);
    if (g_sock_fd >= 0) { close(g_sock_fd); unlink(SOCK_PATH); }
    Nori_Xvision_UnInit();
    printf("\n=== Exit ===\n");
    return 0;
}
