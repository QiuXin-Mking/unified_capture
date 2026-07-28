#pragma once

#include "core/camera_config.h"

#include <cstdint>

struct WristVideoFormat {
    bool is_mjpeg = false;
    int width = 0;
    int height = 0;
    int fps = 0;
};

const WristVideoFormat& wrist_target_format();
CameraConfig make_wrist_left_config(uint16_t vid, uint16_t pid,
                                    uint32_t device_id);
CameraConfig make_wrist_right_config(uint16_t vid, uint16_t pid,
                                     uint32_t device_id);
