#pragma once

#include "hardware/video/device_discovery.h"

#include <vector>

std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras);
