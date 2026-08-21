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
    CameraDiscoveryResult partial_mango;
    partial_mango.profile = ProductProfile::mango;
    partial_mango.wrist[0] = enabled_slot("wrist_left");
    const std::vector<CameraSlot> partial =
        active_profile_cameras(partial_mango);
    assert(partial.size() == 1);
    assert(std::string(partial[0].config.name) == "wrist_left");
    assert(profile_cameras_json(partial_mango) ==
           "\"cameras\":{\"head\":false,\"wrist_left\":true,\"wrist_right\":false}");

    CameraDiscoveryResult empty_mango;
    empty_mango.profile = ProductProfile::mango;
    assert(active_profile_cameras(empty_mango).empty());
    assert(profile_cameras_json(empty_mango) ==
           "\"cameras\":{\"head\":false,\"wrist_left\":false,\"wrist_right\":false}");

    CameraDiscoveryResult banana;
    banana.profile = ProductProfile::banana;
    banana.jhh2[0] = enabled_slot("jhh2_left");
    banana.wrist[0] = enabled_slot("wrist_left");
    const std::vector<CameraSlot> banana_cameras =
        active_profile_cameras(banana);
    assert(banana_cameras.size() == 1);
    assert(std::string(banana_cameras[0].config.name) == "jhh2_left");
    banana.sixcam.enabled = true;
    assert(profile_session_directories(banana) ==
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

    // mango 双目档：active 只含腕部，json 报 head，目录含 head
    CameraDiscoveryResult mango_dual;
    mango_dual.profile = ProductProfile::mango;
    mango_dual.head = enabled_slot("head");
    mango_dual.wrist[0] = enabled_slot("wrist_left");
    assert(active_profile_cameras(mango_dual).size() == 1);
    assert(active_profile_cameras(mango_dual)[0].config.name ==
           std::string("wrist_left"));
    assert(profile_cameras_json(mango_dual) ==
           "\"cameras\":{\"head\":true,\"wrist_left\":true,\"wrist_right\":false}");
    assert(profile_session_directories(mango_dual) ==
           std::vector<std::string>({"wrist_left", "head"}));

    // mango_pro 六目档：json 报 wrist + jhh04/jhh02，目录含 jhh04/jhh02
    CameraDiscoveryResult mango_pro;
    mango_pro.profile = ProductProfile::mango_pro;
    mango_pro.wrist[0] = enabled_slot("wrist_left");
    mango_pro.sixcam.enabled = true;
    mango_pro.sixcam.jhh04_path = "/dev/video6";
    mango_pro.sixcam.jhh02_path = "/dev/video4";
    assert(profile_cameras_json(mango_pro) ==
           "\"cameras\":{\"wrist_left\":true,\"wrist_right\":false,\"jhh04\":true,\"jhh02\":true}");
    assert(profile_session_directories(mango_pro) ==
           std::vector<std::string>({"wrist_left", "jhh04", "jhh02"}));

    return 0;
}
