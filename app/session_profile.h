#pragma once

#include "hardware/video/device_discovery.h"

#include <string>
#include <vector>

std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras);
std::string profile_cameras_json(const CameraDiscoveryResult& cameras);
