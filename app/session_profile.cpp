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
