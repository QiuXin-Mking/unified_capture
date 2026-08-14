#include "app/capture_output_policy.h"

#include <cassert>
#include <string>

int main() {
    assert(camera_pipeline_for_profile(ProductProfile::cherry) ==
           CameraPipeline::cherry_h264_remux);
    assert(camera_pipeline_for_profile(ProductProfile::mango) ==
           CameraPipeline::legacy_mpp);
    assert(camera_pipeline_for_profile(ProductProfile::banana) ==
           CameraPipeline::legacy_mpp);

    assert(profile_video_option_error(ProductProfile::cherry, true).empty());
    const std::string cherry_error =
        profile_video_option_error(ProductProfile::cherry, false);
    assert(cherry_error.find("H.264") != std::string::npos);
    assert(cherry_error.find("mandatory") != std::string::npos);
    assert(cherry_error.find("--no-h265") != std::string::npos);
    assert(profile_video_option_error(ProductProfile::mango, false).find(
               "H.265") != std::string::npos);
    assert(profile_video_option_error(ProductProfile::banana, false).empty());

    const CameraOutputPolicy left = mango_camera_output_policy("wrist_left");
    assert(left.output_h265);
    assert(!left.output_y8);

    const CameraOutputPolicy right = mango_camera_output_policy("wrist_right");
    assert(right.output_h265);
    assert(!right.output_y8);

    const CameraOutputPolicy jhh02 = mango_camera_output_policy("jhh02");
    assert(jhh02.output_h265);
    assert(!jhh02.output_y8);

    const CameraOutputPolicy jhh04 = mango_camera_output_policy("jhh04");
    assert(!jhh04.output_h265);
    assert(!jhh04.output_y8);

    const CameraOutputPolicy unknown = mango_camera_output_policy("unknown");
    assert(!unknown.output_h265);
    assert(!unknown.output_y8);
}
