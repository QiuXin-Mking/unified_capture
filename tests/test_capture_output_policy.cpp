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

    // mango 双目档：head 输出 H.265 + Y8（Y8 经共享内存 socket，不落盘）
    const CameraOutputPolicy head = mango_camera_output_policy("head");
    assert(head.output_h265);
    assert(head.output_y8);

    // mango_pro 六目档：jhh02 无 Y8、jhh04 有 Y8、腕部无 Y8
    const CameraOutputPolicy pro_jhh02 = mango_pro_camera_output_policy("jhh02");
    assert(pro_jhh02.output_h265);
    assert(!pro_jhh02.output_y8);
    const CameraOutputPolicy pro_jhh04 = mango_pro_camera_output_policy("jhh04");
    assert(pro_jhh04.output_h265);
    assert(pro_jhh04.output_y8);
    const CameraOutputPolicy pro_wrist = mango_pro_camera_output_policy("wrist_left");
    assert(pro_wrist.output_h265);
    assert(!pro_wrist.output_y8);

    // mango 双目档不应再认 jhh02/jhh04
    const CameraOutputPolicy mango_no_sixcam = mango_camera_output_policy("jhh02");
    assert(!mango_no_sixcam.output_h265);
    assert(!mango_no_sixcam.output_y8);

    const CameraOutputPolicy unknown = mango_camera_output_policy("unknown");
    assert(!unknown.output_h265);
    assert(!unknown.output_y8);
}
