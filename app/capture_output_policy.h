#pragma once

#include "core/product_config.h"

#include <string>
#include <string_view>

enum class CameraPipeline {
    legacy_mpp,
    cherry_h264_remux,
};

inline CameraPipeline camera_pipeline_for_profile(ProductProfile profile) {
    return profile == ProductProfile::cherry
               ? CameraPipeline::cherry_h264_remux
               : CameraPipeline::legacy_mpp;
}

inline std::string profile_video_option_error(ProductProfile profile,
                                              bool use_h265) {
    if (use_h265) {
        return {};
    }
    if (profile == ProductProfile::cherry) {
        return "cherry H.264 video is mandatory; --no-h265 is unsupported";
    }
    if (profile == ProductProfile::mango) {
        return "mango requires H.265 output";
    }
    return {};
}

struct CameraOutputPolicy {
    bool output_h265 = false;
    bool output_y8 = false;
};

inline CameraOutputPolicy mango_camera_output_policy(std::string_view name) {
    if (name == "wrist_left" || name == "wrist_right" || name == "jhh02") {
        return {true, false};
    }
    if (name == "jhh04") {
        return {true, true};
    }
    return {false, false};
}
