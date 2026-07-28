#include "app/session_runner.h"
#include "core/output_path.h"
#include "core/time_utils.h"
#include "hardware/tracker/vive_usb.h"
#include "hardware/video/device_discovery.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "Nori_Xvision_API.h"
}

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_session_running{false};

// Socket (单线程, 在主循环中轮询)
static const char* SOCK_PATH = "/tmp/unified_capture.sock";
static int g_sock_fd = -1;
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_socket_start_request{false};
static bool g_use_h265 = true;
static bool g_use_vive = true;
static bool g_use_imu = true;
static bool g_use_as5600 = false;

static const char* GPIO_CHIP = "/dev/gpiochip2";
static const int GPIO_BTN_LINE = 8;
#define LED_PATH "/sys/class/leds/sys_led"

static void sig_handler(int sig) {
    if (sig == SIGSEGV || sig == SIGABRT) {
        fprintf(stderr, "\n!!! FATAL: caught signal %d\n", sig);
        fflush(stderr);
        _exit(1);
    }
    g_running = false;
    g_session_running = false;
}

static void led_set(int brightness) {
    int fd = open(LED_PATH "/brightness", O_WRONLY);
    if (fd < 0) {
        return;
    }
    char value = brightness ? '1' : '0';
    ssize_t ignored = write(fd, &value, 1);
    (void)ignored;
    close(fd);
}

static void led_disable_trigger() {
    int fd = open(LED_PATH "/trigger", O_WRONLY);
    if (fd < 0) {
        return;
    }
    ssize_t ignored = write(fd, "none", 4);
    (void)ignored;
    close(fd);
}

static void socket_handle_client(int fd, SessionRunner& sessions) {
    char buffer[256];
    ssize_t size = read(fd, buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return;
    }
    buffer[size] = '\0';
    if (buffer[size - 1] == '\n') {
        buffer[size - 1] = '\0';
    }

    std::string response;
    if (!strcmp(buffer, "start")) {
        if (!g_ready) {
            response = "{\"ok\":false,\"error\":\"not ready\"}";
        } else if (g_session_running) {
            response = "{\"ok\":false,\"error\":\"already running\"}";
        } else {
            g_socket_start_request = true;
            response = "{\"ok\":true}";
        }
    } else if (!strcmp(buffer, "stop")) {
        if (!g_session_running) {
            response = "{\"ok\":false,\"error\":\"not running\"}";
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed =
                (now.tv_sec - g_t0.tv_sec) * 1000 +
                (now.tv_nsec - g_t0.tv_nsec) / 1000000;
            g_session_running = false;
            char result[64];
            snprintf(result, sizeof(result),
                     "{\"ok\":true,\"elapsed_ms\":%ld}", elapsed);
            response = result;
        }
    } else if (!strncmp(buffer, "preview:", 8)) {
        const char* path = buffer + 8;
        if (!g_session_running) {
            response = "{\"ok\":false,\"error\":\"not running\"}";
        } else {
            sessions.request_preview(path);
            response = "{\"ok\":true}";
        }
    } else if (!strcmp(buffer, "status")) {
        if (!g_ready) {
            char result[512];
            snprintf(
                result, sizeof(result),
                "{\"ok\":true,\"ready\":false,\"running\":false,\"session\":null,"
                "\"elapsed_ms\":0,\"cameras\":{},\"imu\":%s,\"as5600\":%s,"
                "\"vive\":%s}",
                g_use_imu ? "true" : "false",
                g_use_as5600 ? "true" : "false",
                g_use_vive ? "true" : "false");
            response = result;
        } else {
            long elapsed = 0;
            if (g_session_running) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                elapsed =
                    (now.tv_sec - g_t0.tv_sec) * 1000 +
                    (now.tv_nsec - g_t0.tv_nsec) / 1000000;
            }
            char result[1024];
            snprintf(
                result, sizeof(result),
                "{\"ok\":true,\"ready\":true,\"running\":%s,\"session\":null,"
                "\"elapsed_ms\":%ld,%s,\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
                g_session_running ? "true" : "false", elapsed,
                sessions.cameras_json().c_str(),
                g_use_imu ? "true" : "false",
                g_use_as5600 ? "true" : "false",
                g_use_vive ? "true" : "false");
            response = result;
        }
    } else {
        response = "{\"ok\":false,\"error\":\"unknown command\"}";
    }
    response += "\n";
    write(fd, response.c_str(), response.size());
}

static int socket_setup() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCK_PATH, sizeof(address.sun_path) - 1);

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) < 0) {
            unlink(SOCK_PATH);
        }
        close(probe);
    }
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    printf("[socket] listening on %s (poll mode)\n", SOCK_PATH);
    return fd;
}

static void print_usage(const char* program) {
    printf("Usage: %s [OPTIONS] [output_prefix]\n", program);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, sig_handler);
    signal(SIGABRT, sig_handler);
    setlinebuf(stdout);
    setlinebuf(stderr);

    bool use_gpio = true;
    bool use_socket = false;
    bool single_shot = false;
    g_use_as5600 = true;
    g_use_imu = true;
    g_use_vive = true;
    std::string prefix;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scan")) {
            scan_devices();
            return 0;
        } else if (!strcmp(argv[i], "--no-gpio")) {
            use_gpio = false;
        } else if (!strcmp(argv[i], "--socket")) {
            use_socket = true;
            use_gpio = false;
        } else if (!strcmp(argv[i], "--no-as5600")) {
            g_use_as5600 = false;
        } else if (!strcmp(argv[i], "--no-imu")) {
            g_use_imu = false;
        } else if (!strcmp(argv[i], "--no-h265")) {
            g_use_h265 = false;
        } else if (!strcmp(argv[i], "--single")) {
            single_shot = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            prefix = argv[i];
        }
    }

    prefix = capture_output_prefix(prefix);
    if (!is_sd_capture_path(prefix)) {
        fprintf(stderr, "ERROR: output must be under %s: %s\n",
                kSdCaptureRoot, prefix.c_str());
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

    if (!g_use_h265) {
        printf("[note] H.265 disabled (--no-h265), output Y8 only\n");
    }

    CameraDiscoveryResult cameras = discover_cameras();
    if (cameras.active_count <= 0) {
        fprintf(stderr, "ERROR: No cameras\n");
        return 1;
    }

    int vive_count = detect_vive_trackers();
    if (vive_count <= 0) {
        g_use_vive = false;
        printf("[vive] no tracker detected, VIVE disabled\n");
    } else {
        g_use_vive = true;
        printf("[vive] %d tracker(s) detected, VIVE enabled\n", vive_count);
    }

    SessionOptions session_options{
        g_use_imu, g_use_as5600, g_use_vive, g_use_h265};
    SessionRunner sessions(cameras, session_options, g_session_running);

    g_ready = true;
    g_sock_fd = socket_setup();

    printf("\n=== Unified Capture (%d camera(s)) ===\n", cameras.active_count);
    led_disable_trigger();
    led_set(0);

    gpiod_chip* chip = nullptr;
    gpiod_line* button = nullptr;
    ControlPump pump_controls = [&](int timeout_ms) {
        struct pollfd descriptors[2];
        int count = 0;
        int button_fd = -1;
        if (button) {
            button_fd = gpiod_line_event_get_fd(button);
            descriptors[count].fd = button_fd;
            descriptors[count].events = POLLIN;
            count++;
        }
        if (g_sock_fd >= 0) {
            descriptors[count].fd = g_sock_fd;
            descriptors[count].events = POLLIN;
            count++;
        }

        int result = poll(descriptors, count, timeout_ms);
        if (result <= 0) {
            return;
        }
        for (int i = 0; i < count; i++) {
            if (!(descriptors[i].revents & POLLIN)) {
                continue;
            }
            if (button && descriptors[i].fd == button_fd) {
                gpiod_line_event event;
                gpiod_line_event_read(button, &event);
                if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                    g_session_running = false;
                }
            } else if (g_sock_fd >= 0 && descriptors[i].fd == g_sock_fd) {
                int client = accept(g_sock_fd, nullptr, nullptr);
                if (client >= 0) {
                    socket_handle_client(client, sessions);
                    close(client);
                }
            }
        }
    };

    if (use_socket) {
        printf("Socket mode. Use 'echo start|nc -U %s' to record.\n\n", SOCK_PATH);
        int session_number = 0;
        for (int candidate = 1; candidate < 999; candidate++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/session_%03d",
                     prefix.c_str(), candidate);
            struct stat status;
            if (stat(path, &status) == 0) {
                session_number = candidate;
            }
        }
        printf("Starting from session_%03d\n", session_number + 1);
        while (g_running) {
            struct pollfd descriptor;
            descriptor.fd = g_sock_fd;
            descriptor.events = POLLIN;
            while (g_running && poll(&descriptor, 1, 200) <= 0) {
            }
            if (!g_running) {
                break;
            }
            int client = accept(g_sock_fd, nullptr, nullptr);
            if (client >= 0) {
                socket_handle_client(client, sessions);
                close(client);
            }
            if (!g_socket_start_request.exchange(false)) {
                continue;
            }

            session_number++;
            g_session_running = true;
            led_set(1);
            std::string dir =
                sessions.make_session_dir(prefix, session_number);
            sessions.run(dir, session_number, pump_controls);
            led_set(0);
            if (single_shot) {
                break;
            }
        }
        if (g_sock_fd >= 0) {
            close(g_sock_fd);
            unlink(SOCK_PATH);
        }
        Nori_Xvision_UnInit();
        return 0;
    }

    if (!use_gpio) {
        printf("Recording... Press Ctrl-C to stop.\n");
        g_session_running = true;
        std::string dir = sessions.make_session_dir(prefix, 1);
        sessions.run(dir, 1, pump_controls);
        if (g_sock_fd >= 0) {
            close(g_sock_fd);
            unlink(SOCK_PATH);
        }
        Nori_Xvision_UnInit();
        return 0;
    }

    chip = gpiod_chip_open(GPIO_CHIP);
    if (chip) {
        button = gpiod_chip_get_line(chip, GPIO_BTN_LINE);
        if (!button ||
            gpiod_line_request_both_edges_events(button, "capture-btn") < 0) {
            if (button) {
                gpiod_line_release(button);
            }
            gpiod_chip_close(chip);
            button = nullptr;
            chip = nullptr;
        }
    }
    if (!button) {
        fprintf(stderr, "GPIO unavailable; use socket start or Ctrl-C\n");
        g_session_running = true;
        std::string dir = sessions.make_session_dir(prefix, 1);
        sessions.run(dir, 1, pump_controls);
        if (g_sock_fd >= 0) {
            close(g_sock_fd);
            unlink(SOCK_PATH);
        }
        printf("\n=== Exit ===\n");
        return 0;
    }

    printf("GPIO ready. Press button or use socket.\n\n");
    int session_number = 0;

    while (g_running) {
        struct pollfd descriptors[2];
        descriptors[0].fd = gpiod_line_event_get_fd(button);
        descriptors[0].events = POLLIN;
        descriptors[1].fd = g_sock_fd;
        descriptors[1].events = POLLIN;
        int result = poll(descriptors, 2, 200);
        if (result < 0 && errno != EINTR) {
            break;
        }

        if (result > 0 && descriptors[1].revents & POLLIN) {
            int client = accept(g_sock_fd, nullptr, nullptr);
            if (client >= 0) {
                socket_handle_client(client, sessions);
                close(client);
            }
        }

        bool start_session = false;
        if (g_socket_start_request.exchange(false)) {
            start_session = true;
        }

        if (!start_session && result > 0 &&
            descriptors[0].revents & POLLIN) {
            gpiod_line_event event;
            if (gpiod_line_event_read(button, &event) < 0) {
                break;
            }
            if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
                start_session = true;
            }
        }

        if (!start_session) {
            continue;
        }

        session_number++;
        g_session_running = true;
        led_set(1);
        std::string dir =
            sessions.make_session_dir(prefix, session_number);
        sessions.run(dir, session_number, pump_controls);
        led_set(0);
        if (single_shot) {
            break;
        }
    }

    if (button) {
        gpiod_line_release(button);
    }
    if (chip) {
        gpiod_chip_close(chip);
    }
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        unlink(SOCK_PATH);
    }
    Nori_Xvision_UnInit();
    printf("\n=== Exit ===\n");
    return 0;
}
