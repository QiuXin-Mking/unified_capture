#include "app/runtime.h"

#include "app/gpio_control.h"
#include "app/session_runner.h"
#include "app/socket_server.h"
#include "core/output_path.h"
#include "core/product_config.h"
#include "core/time_utils.h"
#include "hardware/tracker/vive_usb.h"
#include "hardware/video/device_discovery.h"

#include <cerrno>
#include <cstdio>
#include <poll.h>
#include <sys/stat.h>
#include <utility>

extern "C" {
#include "Nori_Xvision_API.h"
}

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)) {}

std::atomic<bool>& Runtime::keep_running() {
    return keep_running_;
}

std::atomic<bool>& Runtime::session_running() {
    return session_running_;
}

int Runtime::run() {
    if (options_.scan_only) {
        scan_devices();
        return 0;
    }

    const std::string prefix =
        capture_output_prefix(options_.output_prefix);
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

    if (!options_.use_h265) {
        printf("[note] H.265 disabled (--no-h265), output Y8 only\n");
    }

    const ProductConfiguration configuration;
    CameraDiscoveryResult cameras = discover_cameras(configuration);
    if (cameras.active_count <= 0) {
        fprintf(stderr, "ERROR: No cameras\n");
        return 1;
    }

    int vive_count = detect_vive_trackers();
    bool use_vive = vive_count > 0;
    if (!use_vive) {
        printf("[vive] no tracker detected, VIVE disabled\n");
    } else {
        printf("[vive] %d tracker(s) detected, VIVE enabled\n", vive_count);
    }

    SessionOptions session_options{
        options_.use_imu, options_.use_as5600, use_vive, options_.use_h265};
    SessionRunner sessions(cameras, session_options, session_running_);

    bool ready = true;
    bool socket_start_request = false;
    bool button_start_request = false;
    SocketServer socket;
    socket.open();

    GpioControl gpio;
    printf("\n=== Unified Capture (%d camera(s)) ===\n", cameras.active_count);
    gpio.disable_led_trigger();
    gpio.set_led(false);

    bool gpio_available = false;
    if (options_.use_gpio) {
        gpio_available = gpio.open();
    }

    auto handle_socket_command = [&](const SocketCommand& command) {
        if (command.kind == SocketCommandKind::start) {
            if (!ready) {
                return std::string("{\"ok\":false,\"error\":\"not ready\"}");
            }
            if (session_running_) {
                return std::string(
                    "{\"ok\":false,\"error\":\"already running\"}");
            }
            socket_start_request = true;
            return std::string("{\"ok\":true}");
        }

        if (command.kind == SocketCommandKind::stop) {
            if (!session_running_) {
                return std::string(
                    "{\"ok\":false,\"error\":\"not running\"}");
            }
            struct timespec now {};
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed =
                (now.tv_sec - g_t0.tv_sec) * 1000 +
                (now.tv_nsec - g_t0.tv_nsec) / 1000000;
            session_running_ = false;
            char result[64];
            snprintf(result, sizeof(result),
                     "{\"ok\":true,\"elapsed_ms\":%ld}", elapsed);
            return std::string(result);
        }

        if (command.kind == SocketCommandKind::preview) {
            if (!session_running_) {
                return std::string(
                    "{\"ok\":false,\"error\":\"not running\"}");
            }
            sessions.request_preview(command.preview_path);
            return std::string("{\"ok\":true}");
        }

        if (command.kind == SocketCommandKind::status) {
            if (!ready) {
                char result[512];
                snprintf(
                    result, sizeof(result),
                    "{\"ok\":true,\"ready\":false,\"running\":false,"
                    "\"session\":null,\"elapsed_ms\":0,\"cameras\":{},"
                    "\"imu\":%s,\"as5600\":%s,\"vive\":%s}",
                    options_.use_imu ? "true" : "false",
                    options_.use_as5600 ? "true" : "false",
                    use_vive ? "true" : "false");
                return std::string(result);
            }

            long elapsed = 0;
            if (session_running_) {
                struct timespec now {};
                clock_gettime(CLOCK_MONOTONIC, &now);
                elapsed =
                    (now.tv_sec - g_t0.tv_sec) * 1000 +
                    (now.tv_nsec - g_t0.tv_nsec) / 1000000;
            }
            char result[1024];
            snprintf(
                result, sizeof(result),
                "{\"ok\":true,\"ready\":true,\"running\":%s,"
                "\"session\":null,\"elapsed_ms\":%ld,%s,\"imu\":%s,"
                "\"as5600\":%s,\"vive\":%s}",
                session_running_ ? "true" : "false", elapsed,
                sessions.cameras_json().c_str(),
                options_.use_imu ? "true" : "false",
                options_.use_as5600 ? "true" : "false",
                use_vive ? "true" : "false");
            return std::string(result);
        }

        return std::string(
            "{\"ok\":false,\"error\":\"unknown command\"}");
    };

    ControlPump pump_controls = [&](int timeout_ms) {
        struct pollfd descriptors[2] {};
        int count = 0;
        const int button_fd = gpio_available ? gpio.event_fd() : -1;
        if (button_fd >= 0) {
            descriptors[count].fd = button_fd;
            descriptors[count].events = POLLIN;
            count++;
        }
        if (socket.fd() >= 0) {
            descriptors[count].fd = socket.fd();
            descriptors[count].events = POLLIN;
            count++;
        }

        int result = poll(descriptors, count, timeout_ms);
        if (result < 0) {
            if (errno != EINTR) {
                keep_running_ = false;
                session_running_ = false;
            }
            return;
        }
        if (result == 0) {
            return;
        }

        for (int i = 0; i < count; i++) {
            if (!(descriptors[i].revents & POLLIN)) {
                continue;
            }
            if (button_fd >= 0 && descriptors[i].fd == button_fd) {
                ButtonEvent event = gpio.consume_event();
                if (event == ButtonEvent::falling_edge) {
                    if (session_running_) {
                        session_running_ = false;
                    } else {
                        button_start_request = true;
                    }
                } else if (event == ButtonEvent::error) {
                    keep_running_ = false;
                    session_running_ = false;
                }
            } else if (socket.fd() >= 0 &&
                       descriptors[i].fd == socket.fd()) {
                socket.serve_one(handle_socket_command);
            }
        }
    };

    if (options_.socket_mode) {
        printf("Socket mode. Use 'echo start|nc -U %s' to record.\n\n",
               "/tmp/unified_capture.sock");
        int session_number = 0;
        for (int candidate = 1; candidate < 999; candidate++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/session_%03d",
                     prefix.c_str(), candidate);
            struct stat status {};
            if (stat(path, &status) == 0) {
                session_number = candidate;
            }
        }
        printf("Starting from session_%03d\n", session_number + 1);

        while (keep_running_) {
            pump_controls(200);
            if (!keep_running_) {
                break;
            }
            if (!socket_start_request) {
                continue;
            }
            socket_start_request = false;

            session_number++;
            session_running_ = true;
            gpio.set_led(true);
            std::string dir =
                sessions.make_session_dir(prefix, session_number);
            sessions.run(dir, session_number, pump_controls);
            gpio.set_led(false);
            if (options_.single_shot) {
                break;
            }
        }
    } else if (!options_.use_gpio) {
        printf("Recording... Press Ctrl-C to stop.\n");
        session_running_ = true;
        std::string dir = sessions.make_session_dir(prefix, 1);
        sessions.run(dir, 1, pump_controls);
    } else if (!gpio_available) {
        fprintf(stderr, "GPIO unavailable; use socket start or Ctrl-C\n");
        session_running_ = true;
        std::string dir = sessions.make_session_dir(prefix, 1);
        sessions.run(dir, 1, pump_controls);
    } else {
        printf("GPIO ready. Press button or use socket.\n\n");
        int session_number = 0;
        while (keep_running_) {
            pump_controls(200);
            if (!keep_running_) {
                break;
            }

            bool start_session = socket_start_request;
            socket_start_request = false;
            if (button_start_request) {
                start_session = true;
                button_start_request = false;
            }
            if (!start_session) {
                continue;
            }

            session_number++;
            session_running_ = true;
            gpio.set_led(true);
            std::string dir =
                sessions.make_session_dir(prefix, session_number);
            sessions.run(dir, session_number, pump_controls);
            gpio.set_led(false);
            if (options_.single_shot) {
                break;
            }
        }
    }

    gpio.close();
    socket.close();
    Nori_Xvision_UnInit();
    if (options_.use_gpio) {
        printf("\n=== Exit ===\n");
    }
    return 0;
}
