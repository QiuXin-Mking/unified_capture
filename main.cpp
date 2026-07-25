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
#include <poll.h>
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

// ============================================================
// 全局状态
// ============================================================
struct timespec g_t0;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_session_running{false};
std::atomic<int> g_jhh2_remaining{0};

// Socket (单线程, 在主循环中轮询)
static const char* SOCK_PATH = "/tmp/unified_capture.sock";
static int g_sock_fd = -1;
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_socket_start_request{false};
static bool g_use_h265 = true;

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
    v4l2_dev_sys_data_t* dev_ptr = nullptr;
};

static CamEntry CAMS[] = {
{{"jhh2_left",  JHH2_VID, JHH2_PID, 0, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
{{"jhh2_right", JHH2_VID, JHH2_PID, 1, 3840, 1200, 30, 16000000, 30, true,  ImuOrientation::HORIZONTAL_TOP, true,  true},  true, nullptr},
};

struct SixCamEntry {
    bool enabled = true;
    v4l2_dev_sys_data_t* jhh04_dev = nullptr;
    v4l2_dev_sys_data_t* jhh02_dev = nullptr;
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
    {
        v4l2_dev_sys_data_t* devs = nullptr;
        int n = TST_USBCam_DEVICE_FIND_ID(&devs, JHH2_VID, JHH2_PID);
        printf("JHH2 组 [%04x:%04x]: %d device(s)\n", JHH2_VID, JHH2_PID, n);
        for (int i = 0; i < n; i++)
            printf("  [group:%d] %s %s  (%s)\n", i, devs[i].iManufacturer, devs[i].iProduct, devs[i].Device_Path);
    }
    {
        v4l2_dev_sys_data_t* devs = nullptr;
        int n = TST_USBCam_DEVICE_FIND_ID(&devs, SIX_VID, SIX_PID);
        printf("六目组 [%04x:%04x]: %d device(s)\n", SIX_VID, SIX_PID, n);
        for (int i = 0; i < n; i++)
            printf("  [group:%d] %s %s  (%s)\n", i, devs[i].iManufacturer, devs[i].iProduct, devs[i].Device_Path);
    }
}

struct VidPidGroup { uint16_t vid, pid; v4l2_dev_sys_data_t* devs; int count; };

static int resolve_camera_devices() {
    std::vector<VidPidGroup> groups;
    for (int i = 0; i < N_CAMS; i++) {
        auto& cam = CAMS[i];
        if (!cam.enabled) continue;
        uint16_t vid = cam.cfg.vid, pid = cam.cfg.pid;
        VidPidGroup* grp = nullptr;
        for (auto& g : groups) { if (g.vid == vid && g.pid == pid) { grp = &g; break; } }
        if (!grp) {
            VidPidGroup g; g.vid = vid; g.pid = pid;
            g.count = TST_USBCam_DEVICE_FIND_ID(&g.devs, vid, pid);
            groups.push_back(g); grp = &groups.back();
        }
        int order = cam.cfg.group_order;
        if (order >= grp->count) {
            cam.enabled = false; cam.dev_ptr = nullptr;
            fprintf(stderr, "WARN: %s [%04x:%04x order=%d] not found\n", cam.cfg.name, vid, pid, order);
            continue;
        }
        cam.dev_ptr = &grp->devs[order];
        printf("  %-12s -> [%04x:%04x group:%d] %s  %dx%d@%d IMU=%c\n",
               cam.cfg.name, vid, pid, order, cam.dev_ptr->Device_Path,
               cam.cfg.width, cam.cfg.height, cam.cfg.fps, cam.cfg.has_imu ? 'Y' : 'N');
    }
    int active = 0;
    for (int i = 0; i < N_CAMS; i++) if (CAMS[i].enabled) active++;

    {
        v4l2_dev_sys_data_t* six_devs = nullptr;
        int six_n = TST_USBCam_DEVICE_FIND_ID(&six_devs, SIX_VID, SIX_PID);
        if (six_n > 0) {
            g_sixcam.jhh04_dev = &six_devs[0]; g_sixcam.enabled = true;
            printf("  %-12s -> [%04x:%04x group:0] %s  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh04", SIX_VID, SIX_PID, g_sixcam.jhh04_dev->Device_Path);
            active++;
        } else { g_sixcam.enabled = false; }

        VidPidGroup* jhh2_grp = nullptr;
        for (auto& g : groups) { if (g.vid == JHH2_VID && g.pid == JHH2_PID) { jhh2_grp = &g; break; } }
        if (!jhh2_grp) {
            VidPidGroup g; g.vid = JHH2_VID; g.pid = JHH2_PID;
            g.count = TST_USBCam_DEVICE_FIND_ID(&g.devs, JHH2_VID, JHH2_PID);
            groups.push_back(g); jhh2_grp = &groups.back();
        }
        if (g_sixcam.enabled && jhh2_grp->count >= 3) {
            g_sixcam.jhh02_dev = &jhh2_grp->devs[2];
            printf("  %-12s -> [%04x:%04x group:2] %s  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh02", JHH2_VID, JHH2_PID, g_sixcam.jhh02_dev->Device_Path);
        } else if (g_sixcam.enabled) {
            fprintf(stderr, "WARN: jhh02 not found\n"); g_sixcam.jhh02_dev = nullptr;
        }
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

static std::string default_prefix() {
    time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
    char buf[64]; snprintf(buf, sizeof(buf), "record_%04d%02d%02d_%02d%02d%02d",
        tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
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
        s += ",\"jhh04\":" + std::string(g_sixcam.jhh04_dev ? "true" : "false");
        s += ",\"jhh02\":" + std::string(g_sixcam.jhh02_dev ? "true" : "false");
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
    } else if (!strcmp(buf, "status")) {
        if (!g_ready) resp = "{\"ok\":true,\"ready\":false,\"running\":false,\"session\":null,\"elapsed_ms\":0,\"cameras\":{},\"imu\":false,\"as5600\":false,\"vive\":false}";
        else {
            long el = 0;
            if (g_session_running) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                el = (now.tv_sec-g_t0.tv_sec)*1000 + (now.tv_nsec-g_t0.tv_nsec)/1000000; }
            char b[1024]; snprintf(b,sizeof(b),
                "{\"ok\":true,\"ready\":true,\"running\":%s,\"session\":null,\"elapsed_ms\":%ld,%s,\"imu\":true,\"as5600\":false,\"vive\":false}",
                g_session_running?"true":"false", el, cameras_json().c_str());
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
    std::vector<Sensor*> sensors;

    g_jhh2_remaining = 0;
    for (int i = 0; i < N_CAMS; i++)
        if (CAMS[i].enabled && CAMS[i].cfg.vid==JHH2_VID && CAMS[i].cfg.pid==JHH2_PID) g_jhh2_remaining++;
    if (g_sixcam.enabled && g_sixcam.jhh02_dev) g_jhh2_remaining++;

    for (int i = 0; i < N_CAMS; i++) {
        if (!CAMS[i].enabled) continue;
        auto& cam = CAMS[i];
        auto* vs = new VideoSensor(cam.cfg, ses_dir, *cam.dev_ptr, session_num, g_session_running);
        sensors.push_back(vs);
        if (use_imu && cam.cfg.has_imu)
            sensors.push_back(new ImuSensor(cam.cfg.name, ses_dir, vs->imu_queue(), session_num, cam.cfg.imu_orientation, g_session_running));
    }
    if (g_sixcam.enabled && g_sixcam.jhh04_dev && g_sixcam.jhh02_dev) {
        CameraConfig j04{"jhh04",SIX_VID,SIX_PID,0,3104,480,30,4000000,30,true,ImuOrientation::HORIZONTAL_TOP,false,true};
        CameraConfig j02{"jhh02",JHH2_VID,JHH2_PID,2,4000,1200,30,16000000,30,true,ImuOrientation::HORIZONTAL_TOP,g_use_h265,true};
        auto* sc = new SixCamSensor(j04,j02,*g_sixcam.jhh04_dev,*g_sixcam.jhh02_dev,ses_dir,session_num,g_session_running);
        sensors.push_back(sc);
        if (use_imu) {
            sensors.push_back(new ImuSensor("jhh04",ses_dir,sc->imu_queue_jhh04(),session_num,ImuOrientation::HORIZONTAL_TOP,g_session_running));
            sensors.push_back(new ImuSensor("jhh02",ses_dir,sc->imu_queue_jhh02(),session_num,ImuOrientation::HORIZONTAL_TOP,g_session_running));
        }
    }
    if (use_as5600) sensors.push_back(new EncoderSensor(ENC_I2C_PATH,ENC_I2C_ADDR,ses_dir,session_num,ENC_INTERVAL_US,g_session_running));
    if (use_vive) sensors.push_back(new ViveTrackerSensor(ses_dir,session_num,g_session_running));

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

    bool use_gpio=true, use_socket=false, use_as5600=true, use_imu=true, use_vive=true, single_shot=false;
    std::string prefix;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--scan")) { scan_devices(); return 0; }
        else if (!strcmp(argv[i],"--no-gpio")) use_gpio=false;
        else if (!strcmp(argv[i],"--socket")) { use_socket=true; use_gpio=false; }
        else if (!strcmp(argv[i],"--no-as5600")) use_as5600=false;
        else if (!strcmp(argv[i],"--no-imu")) use_imu=false;
        else if (!strcmp(argv[i],"--no-vive")) use_vive=false;
        else if (!strcmp(argv[i],"--no-h265")) g_use_h265=false;
        else if (!strcmp(argv[i],"--single")) single_shot=true;
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) { print_usage(argv[0]); return 0; }
        else if (argv[i][0]!='-') prefix=argv[i];
    }
    if (prefix.empty()) prefix = default_prefix();

    // --no-h265: 全局禁用 H.265 编码 (排查用)
    if (!g_use_h265) {
        for (int i = 0; i < N_CAMS; i++) CAMS[i].cfg.output_h265 = false;
        printf("[note] H.265 disabled (--no-h265), output Y8 only\n");
    }

    int active = resolve_camera_devices();
    if (active <= 0) { fprintf(stderr,"ERROR: No cameras\n"); return 1; }
    g_ready = true;

    // ★ Socket 在主线程建立 (无额外线程, 避免干扰 TSTC/MPP)
    g_sock_fd = socket_setup();

    printf("\n=== Unified Capture (%d camera(s)) ===\n", active);
    led_disable_trigger(); led_set(0);

    // --socket 模式: 纯 socket 控制, 无限循环等 start
    if (use_socket) {
        printf("Socket mode. Use 'echo start|nc -U %s' to record.\n\n", SOCK_PATH);
        int session_num = 0;
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
            run_session(sd, session_num, use_imu, use_as5600, use_vive, nullptr);
            led_set(0);
        }
        if (g_sock_fd>=0) { close(g_sock_fd); unlink(SOCK_PATH); }
        return 0;
    }

    // --no-gpio 模式: 启动即录
    if (!use_gpio) {
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_running = true;
        std::string sd = make_session_dir(prefix, 1);
        run_session(sd, 1, use_imu, use_as5600, use_vive, nullptr);
        if (g_sock_fd>=0) { close(g_sock_fd); unlink(SOCK_PATH); }
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
        run_session(sd,1,use_imu,use_as5600,use_vive,nullptr);
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
        run_session(sd, session_num, use_imu, use_as5600, use_vive, btn);
        led_set(0);
        if (single_shot) break;
    }

    if (btn) gpiod_line_release(btn);
    if (chip) gpiod_chip_close(chip);
    if (g_sock_fd >= 0) { close(g_sock_fd); unlink(SOCK_PATH); }
    printf("\n=== Exit ===\n");
    return 0;
}
