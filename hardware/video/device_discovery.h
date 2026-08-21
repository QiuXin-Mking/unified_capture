#pragma once

#include "core/camera_config.h"
#include "core/product_config.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct CameraSlot {
    CameraConfig config;
    bool enabled = false;
    std::string device_path;
};

struct SixCamDevices {
    bool enabled = false;
    std::string jhh04_path;
    std::string jhh02_path;
};

struct CherryDevices {
    CameraSlot stereo;
    std::string serial_path;
    std::string usb_parent;
};

struct CameraDiscoveryResult {
    ProductProfile profile = ProductProfile::mango;
    std::array<CameraSlot, 2> jhh2;
    SixCamDevices sixcam;
    CameraSlot head;
    std::array<CameraSlot, 2> wrist;
    CherryDevices cherry;
    bool degraded = false;
    std::vector<std::string> camera_errors;
    int active_count = 0;
};

void scan_devices();
CameraDiscoveryResult discover_cameras(const ProductConfiguration& configuration);
