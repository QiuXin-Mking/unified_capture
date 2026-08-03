#pragma once

#include "hardware/video/device_discovery.h"

#include <string>
#include <vector>

enum class CherrySensorRole {
    serial,
    video,
};

std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras);
std::string profile_cameras_json(const CameraDiscoveryResult& cameras);
std::vector<std::string> profile_session_directories(
    const CameraDiscoveryResult& cameras);
std::vector<CherrySensorRole> cherry_sensor_roles(
    const CameraDiscoveryResult& cameras);
