#include "app/session_profile.h"

std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras) {
    std::vector<CameraSlot> active;
    if (cameras.profile == ProductProfile::cherry) {
        if (cameras.cherry.stereo.enabled) {
            active.push_back(cameras.cherry.stereo);
        }
        return active;
    }

    const auto& slots = cameras.profile == ProductProfile::banana
                            ? cameras.wrist
                            : cameras.jhh2;
    for (const CameraSlot& slot : slots) {
        if (slot.enabled) {
            active.push_back(slot);
        }
    }
    return active;
}

std::string profile_cameras_json(const CameraDiscoveryResult& cameras) {
    if (cameras.profile == ProductProfile::cherry) {
        return "\"cameras\":{\"cherry_stereo\":" +
               std::string(cameras.cherry.stereo.enabled ? "true" : "false") +
               "}";
    }

    if (cameras.profile == ProductProfile::banana) {
        return "\"cameras\":{\"wrist_left\":" +
               std::string(cameras.wrist[0].enabled ? "true" : "false") +
               ",\"wrist_right\":" +
               std::string(cameras.wrist[1].enabled ? "true" : "false") + "}";
    }

    std::string json = "\"cameras\":{";
    bool first = true;
    for (const CameraSlot& camera : cameras.jhh2) {
        if (!first) {
            json += ",";
        }
        json += "\"" + std::string(camera.config.name) + "\":" +
                (camera.enabled ? "true" : "false");
        first = false;
    }
    if (cameras.sixcam.enabled) {
        json += ",\"jhh04\":" +
                std::string(!cameras.sixcam.jhh04_path.empty() ? "true" : "false");
        json += ",\"jhh02\":" +
                std::string(!cameras.sixcam.jhh02_path.empty() ? "true" : "false");
    }
    json += "}";
    return json;
}

std::vector<CherrySensorRole> cherry_sensor_roles(
    const CameraDiscoveryResult& cameras) {
    if (cameras.profile != ProductProfile::cherry ||
        !cameras.cherry.stereo.enabled ||
        cameras.cherry.stereo.device_path.empty() ||
        cameras.cherry.serial_path.empty()) {
        return {};
    }
    return {CherrySensorRole::serial, CherrySensorRole::video};
}

std::vector<std::string> profile_session_directories(
    const CameraDiscoveryResult& cameras) {
    std::vector<std::string> directories;
    for (const CameraSlot& camera : active_profile_cameras(cameras)) {
        directories.emplace_back(camera.config.name);
    }
    if (cameras.profile == ProductProfile::cherry) {
        return directories;
    }
    if (cameras.sixcam.enabled) {
        directories.emplace_back("jhh04");
        directories.emplace_back("jhh02");
    }
    return directories;
}
