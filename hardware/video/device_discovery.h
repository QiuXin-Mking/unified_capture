#pragma once

#include "core/camera_config.h"

#include <array>
#include <cstdint>

struct CameraSlot {
    CameraConfig config;
    bool enabled = false;
};

struct SixCamDevices {
    bool enabled = false;
    uint32_t jhh04_id = 0;
    uint32_t jhh02_id = 0;
};

struct CameraDiscoveryResult {
    std::array<CameraSlot, 2> jhh2;
    SixCamDevices sixcam;
    int active_count = 0;
};

void scan_devices();
CameraDiscoveryResult discover_cameras();
