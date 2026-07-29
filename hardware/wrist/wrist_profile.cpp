#include "hardware/wrist/wrist_profile.h"

namespace {

constexpr WristVideoFormat kTargetFormat{true, 1440, 960, 30};

CameraConfig make_wrist_config(const char* name, uint16_t vid, uint16_t pid,
                               int group_order) {
    return CameraConfig{name, vid, pid, group_order, 1440, 960, 30,
                        8000000, 30, true, ImuOrientation::HORIZONTAL_TOP,
                        true, false};
}

}  // namespace

const WristVideoFormat& wrist_target_format() {
    return kTargetFormat;
}

CameraConfig make_wrist_left_config(uint16_t vid, uint16_t pid) {
    return make_wrist_config("wrist_left", vid, pid, 0);
}

CameraConfig make_wrist_right_config(uint16_t vid, uint16_t pid) {
    return make_wrist_config("wrist_right", vid, pid, 1);
}
