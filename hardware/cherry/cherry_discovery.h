#pragma once

#include "core/product_config.h"

#include <cstdint>
#include <string>
#include <vector>

struct CherryVideoEndpoint {
    std::string device_path;
    std::string usb_parent;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint32_t bus = 0;
    bool supports_target = false;
};

struct CherrySerialEndpoint {
    std::string device_path;
    std::string usb_parent;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint32_t bus = 0;
};

struct CherryDiscoveryResult {
    bool available = false;
    std::string video_path;
    std::string serial_path;
    std::string usb_parent;
    uint32_t bus = 0;
    std::string error;
};

CherryDiscoveryResult match_cherry_device(
    const CherryDeviceMap& device_map,
    const std::vector<CherryVideoEndpoint>& videos,
    const std::vector<CherrySerialEndpoint>& serials);

std::vector<CherryVideoEndpoint> scan_cherry_video_endpoints();
std::vector<CherrySerialEndpoint> scan_cherry_serial_endpoints();
CherryDiscoveryResult discover_cherry_device(const CherryDeviceMap& device_map);
