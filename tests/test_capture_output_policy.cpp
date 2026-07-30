#include "app/capture_output_policy.h"

#include <cassert>

int main() {
    const CameraOutputPolicy left = banana_camera_output_policy("wrist_left");
    assert(left.output_h265);
    assert(!left.output_y8);

    const CameraOutputPolicy right = banana_camera_output_policy("wrist_right");
    assert(right.output_h265);
    assert(!right.output_y8);

    const CameraOutputPolicy jhh02 = banana_camera_output_policy("jhh02");
    assert(jhh02.output_h265);
    assert(!jhh02.output_y8);

    const CameraOutputPolicy jhh04 = banana_camera_output_policy("jhh04");
    assert(!jhh04.output_h265);
    assert(!jhh04.output_y8);

    const CameraOutputPolicy unknown = banana_camera_output_policy("unknown");
    assert(!unknown.output_h265);
    assert(!unknown.output_y8);
}
