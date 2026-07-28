#include "hardware/wrist/wrist_profile.h"

namespace {

constexpr WristVideoFormat kTargetFormat{true, 1440, 960, 30};

CameraConfig make_wrist_config(const char* name, uint16_t vid, uint16_t pid,
                               int group_order, uint32_t device_id) {
    return CameraConfig{name, vid, pid, group_order, 1440, 960, 30,
                        8000000, 30, true, ImuOrientation::VERTICAL_LEFT,
                        true, false, static_cast<int>(device_id)};
}

}  // namespace

const WristVideoFormat& wrist_target_format() {
    return kTargetFormat;
}

CameraConfig make_wrist_left_config(uint16_t vid, uint16_t pid,
                                    uint32_t device_id) {
    return make_wrist_config("wrist_left", vid, pid, 0, device_id);
}

CameraConfig make_wrist_right_config(uint16_t vid, uint16_t pid,
                                     uint32_t device_id) {
    return make_wrist_config("wrist_right", vid, pid, 1, device_id);
}
