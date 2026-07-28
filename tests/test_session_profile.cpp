#include "app/session_profile.h"

#include <cassert>
#include <string>

namespace {

CameraSlot enabled_slot(const char* name) {
    CameraSlot slot;
    slot.config.name = name;
    slot.enabled = true;
    return slot;
}

}  // namespace

int main() {
    CameraDiscoveryResult partial_banana;
    partial_banana.profile = ProductProfile::banana;
    partial_banana.wrist[0] = enabled_slot("wrist_left");
    const std::vector<CameraSlot> partial =
        active_profile_cameras(partial_banana);
    assert(partial.size() == 1);
    assert(std::string(partial[0].config.name) == "wrist_left");

    CameraDiscoveryResult empty_banana;
    empty_banana.profile = ProductProfile::banana;
    assert(active_profile_cameras(empty_banana).empty());

    CameraDiscoveryResult mango;
    mango.profile = ProductProfile::mango;
    mango.jhh2[0] = enabled_slot("jhh2_left");
    mango.wrist[0] = enabled_slot("wrist_left");
    const std::vector<CameraSlot> mango_cameras =
        active_profile_cameras(mango);
    assert(mango_cameras.size() == 1);
    assert(std::string(mango_cameras[0].config.name) == "jhh2_left");
    return 0;
}
