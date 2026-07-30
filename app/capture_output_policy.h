#pragma once

#include <string_view>

struct CameraOutputPolicy {
    bool output_h265 = false;
    bool output_y8 = false;
};

inline CameraOutputPolicy banana_camera_output_policy(std::string_view name) {
    if (name == "wrist_left" || name == "wrist_right" || name == "jhh02") {
        return {true, false};
    }
    return {false, false};
}
