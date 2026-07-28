#pragma once

#include <atomic>
#include <string>

struct RuntimeOptions {
    bool scan_only = false;
    std::string output_prefix;
    bool use_gpio = true;
    bool socket_mode = false;
    bool single_shot = false;
    bool use_imu = true;
    bool use_as5600 = true;
    bool use_h265 = true;
    std::string product_config_path = "/etc/unified_capture/product.conf";
    std::string camera_map_path = "/etc/unified_capture/camera-map.conf";
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options);

    int run();
    std::atomic<bool>& keep_running();
    std::atomic<bool>& session_running();

private:
    RuntimeOptions options_;
    std::atomic<bool> keep_running_{true};
    std::atomic<bool> session_running_{false};
};
