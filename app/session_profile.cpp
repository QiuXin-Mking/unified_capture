#include "app/session_profile.h"

std::vector<CameraSlot> active_profile_cameras(
    const CameraDiscoveryResult& cameras) {
    std::vector<CameraSlot> active;
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
                std::string(cameras.sixcam.jhh04_id > 0 ? "true" : "false");
        json += ",\"jhh02\":" +
                std::string(cameras.sixcam.jhh02_id > 0 ? "true" : "false");
    }
    json += "}";
    return json;
}
