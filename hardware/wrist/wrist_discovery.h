#pragma once

#include "core/product_config.h"
#include "hardware/wrist/wrist_profile.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct WristDeviceInfo {
    uint32_t device_id = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string product;
    std::vector<WristVideoFormat> formats;
};

struct WristCameraSlot {
    CameraConfig config{};
    bool available = false;
    std::string error;
};

struct WristDiscoveryResult {
    std::array<WristCameraSlot, 2> cameras;
    bool degraded = false;
    std::vector<std::string> errors;
    int active_count = 0;
};

WristDiscoveryResult match_wrist_cameras(
    const WristDeviceMap& device_map,
    const std::vector<WristDeviceInfo>& inventory);
