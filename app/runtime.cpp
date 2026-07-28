#include "app/runtime.h"

#include "app/gpio_control.h"
#include "app/session_runner.h"
#include "app/socket_server.h"
#include "app/status_response.h"
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
#include <vector>

#include "Nori_Xvision_API.h"

namespace {

std::vector<std::pair<std::string, bool>> status_cameras(
    const CameraDiscoveryResult& cameras) {
    std::vector<std::pair<std::string, bool>> result;
    if (cameras.profile == ProductProfile::banana) {
        result.emplace_back("wrist_left", cameras.wrist[0].enabled);
        result.emplace_back("wrist_right", cameras.wrist[1].enabled);
        if (cameras.sixcam.enabled) {
            result.emplace_back("jhh04", cameras.sixcam.jhh04_id > 0);
            result.emplace_back("jhh02", cameras.sixcam.jhh02_id > 0);
        }
        return result;
    }

    for (const CameraSlot& camera : cameras.jhh2) {
        result.emplace_back(camera.config.name, camera.enabled);
    }
    if (cameras.sixcam.enabled) {
        result.emplace_back("jhh04", cameras.sixcam.jhh04_id > 0);
        result.emplace_back("jhh02", cameras.sixcam.jhh02_id > 0);
    }
    return result;
}

long current_elapsed_ms(const std::atomic<bool>& session_running) {
    if (!session_running) {
        return 0;
    }
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_t0.tv_sec) * 1000 +
           (now.tv_nsec - g_t0.tv_nsec) / 1000000;
}

// ceil(now) + 1 秒  → 保证至少 1 秒缓冲，避免 now=3.999s 时只等 1ms
std::chrono::steady_clock::time_point ceil_to_next_second() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    auto secs = duration_cast<seconds>(now.time_since_epoch());
    // 跳到下一整秒 + 1s 保底
    return steady_clock::time_point(secs + seconds(2));
}

}  // namespace

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

    ProductConfigResult configuration_result = load_product_configuration(
        options_.product_config_path, options_.camera_map_path);
    if (!configuration_result.configuration) {
        fprintf(stderr, "ERROR: %s\n", configuration_result.error.c_str());
        return 2;
    }
    const ProductConfiguration configuration = *configuration_result.configuration;
    const bool is_banana = configuration.profile == ProductProfile::banana;
    if (is_banana && !options_.use_h265) {
        fprintf(stderr, "ERROR: banana requires H.265 output\n");
        return 2;
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

    CameraDiscoveryResult cameras = discover_cameras(configuration);
    if (!is_banana && cameras.active_count <= 0) {
        fprintf(stderr, "ERROR: No cameras\n");
        Nori_Xvision_UnInit();
        return 1;
    }
    if (is_banana && !configuration.wrist.allow_missing_devices) {
        int expected = static_cast<int>(cameras.wrist.size());
        if (configuration.sixcam_enabled) expected += 2;
        if (cameras.active_count != expected) {
            fprintf(stderr, "ERROR: banana requires all devices (wrist x2 + sixcam)\n");
            for (const std::string& error : cameras.camera_errors) {
                fprintf(stderr, "  %s\n", error.c_str());
            }
            Nori_Xvision_UnInit();
            return 1;
        }
    }

    if (is_banana && options_.use_imu) {
        bool any_imu = false;
        for (const auto& cam : cameras.wrist) {
            if (cam.enabled && cam.config.has_imu) any_imu = true;
        }
        if (!any_imu) {
            options_.use_imu = false;
            printf("[imu] wrist cameras have no IMU, IMU disabled\n");
        }
    }

    bool use_vive = false;
    if (is_banana) {
        printf("[vive] banana profile, VIVE disabled\n");
    } else {
        int vive_count = detect_vive_trackers();
        use_vive = vive_count > 0;
        if (!use_vive) {
            printf("[vive] no tracker detected, VIVE disabled\n");
        } else {
            printf("[vive] %d tracker(s) detected, VIVE enabled\n", vive_count);
        }
    }

    const bool use_as5600 = is_banana ? false : options_.use_as5600;

    SessionOptions session_options{
        options_.use_imu, use_as5600, use_vive, options_.use_h265};
    SessionRunner sessions(cameras, session_options, session_running_);

    bool ready = true;
    SocketServer socket;
    socket.open();

    // 前向声明, 供 handle_socket_command 中 stop 阻塞等待用
    ControlPump pump_controls;

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
            target_start_time_ = ceil_to_next_second();
            return std::string("{\"ok\":true}");
        }

        if (command.kind == SocketCommandKind::stop) {
            if (!session_running_) {
                return std::string(
                    "{\"ok\":false,\"error\":\"not running\"}");
            }
            if (target_stop_time_) {
                return std::string(
                    "{\"ok\":false,\"error\":\"stop already scheduled\"}");
            }
            target_stop_time_ = ceil_to_next_second();
            // 阻塞等待 session 真正结束 (teardown 完成, MKV 收尾)
            while (session_running_) {
                pump_controls(50);
            }
            sessions.wait_teardown();
            const long elapsed = current_elapsed_ms(session_running_);
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
            CaptureStatusResponse status{
                std::string(product_profile_name(configuration.profile)),
                ready,
                cameras.degraded,
                session_running_,
                current_elapsed_ms(session_running_),
                status_cameras(cameras),
                cameras.camera_errors,
                options_.use_imu,
                use_as5600,
                use_vive};
            return make_capture_status_json(status);
        }

        return std::string(
            "{\"ok\":false,\"error\":\"unknown command\"}");
    };

    pump_controls = [&](int timeout_ms) {
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
        }
        // result == 0 (timeout) or result > 0: 都继续处理

        for (int i = 0; i < count; i++) {
            if (!(descriptors[i].revents & POLLIN)) {
                continue;
            }
            if (button_fd >= 0 && descriptors[i].fd == button_fd) {
                ButtonEvent event = gpio.consume_event();
                if (event == ButtonEvent::falling_edge) {
                    if (session_running_) {
                        // 整秒对齐停止
                        target_stop_time_ = ceil_to_next_second();
                    } else {
                        // 整秒对齐启动
                        target_start_time_ = ceil_to_next_second();
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

        // 检查是否到达整秒停止时间
        if (target_stop_time_ &&
            std::chrono::steady_clock::now() >= *target_stop_time_) {
            target_stop_time_.reset();
            session_running_ = false;
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
            if (!target_start_time_) {
                continue;
            }
            // 等待到整秒 → sensor 同步启流
            while (std::chrono::steady_clock::now() < *target_start_time_) {
                pump_controls(50);
            }
            target_start_time_.reset();

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
            if (!target_start_time_) {
                continue;
            }
            // 等待到整秒 → sensor 同步启流
            while (std::chrono::steady_clock::now() < *target_start_time_) {
                pump_controls(50);
            }
            target_start_time_.reset();

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
