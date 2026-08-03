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
    assert(profile_cameras_json(partial_banana) ==
           "\"cameras\":{\"wrist_left\":true,\"wrist_right\":false}");

    CameraDiscoveryResult empty_banana;
    empty_banana.profile = ProductProfile::banana;
    assert(active_profile_cameras(empty_banana).empty());
    assert(profile_cameras_json(empty_banana) ==
           "\"cameras\":{\"wrist_left\":false,\"wrist_right\":false}");

    CameraDiscoveryResult mango;
    mango.profile = ProductProfile::mango;
    mango.jhh2[0] = enabled_slot("jhh2_left");
    mango.wrist[0] = enabled_slot("wrist_left");
    const std::vector<CameraSlot> mango_cameras =
        active_profile_cameras(mango);
    assert(mango_cameras.size() == 1);
    assert(std::string(mango_cameras[0].config.name) == "jhh2_left");
    mango.sixcam.enabled = true;
    assert(profile_session_directories(mango) ==
           std::vector<std::string>({"jhh2_left", "jhh04", "jhh02"}));

    CameraDiscoveryResult empty_cherry;
    empty_cherry.profile = ProductProfile::cherry;
    assert(profile_cameras_json(empty_cherry) ==
           "\"cameras\":{\"cherry_stereo\":false}");

    CameraDiscoveryResult cherry;
    cherry.profile = ProductProfile::cherry;
    cherry.cherry.stereo = enabled_slot("cherry_stereo");
    cherry.cherry.stereo.device_path = "/dev/video0";
    cherry.cherry.serial_path = "/dev/ttyACM0";
    cherry.jhh2[0] = enabled_slot("jhh2_left");
    cherry.wrist[0] = enabled_slot("wrist_left");
    cherry.sixcam.enabled = true;
    const std::vector<CameraSlot> cherry_cameras =
        active_profile_cameras(cherry);
    assert(cherry_cameras.size() == 1);
    assert(std::string(cherry_cameras[0].config.name) == "cherry_stereo");
    assert(profile_cameras_json(cherry) ==
           "\"cameras\":{\"cherry_stereo\":true}");
    assert(profile_session_directories(cherry) ==
           std::vector<std::string>({"cherry_stereo"}));

    const std::vector<CherrySensorRole> cherry_roles =
        cherry_sensor_roles(cherry);
    assert(cherry_roles == std::vector<CherrySensorRole>({
        CherrySensorRole::serial, CherrySensorRole::video}));

    cherry.cherry.serial_path.clear();
    assert(cherry_sensor_roles(cherry).empty());
    return 0;
}
