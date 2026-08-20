#include "hardware/video/video_input_format.h"

#include <cassert>
int main() {
    assert(v4l2_pixel_format(sixcam_input_format("jhh04")) ==
           kV4l2PixFmtYuyv);
    assert(v4l2_pixel_format(sixcam_input_format("jhh02")) ==
           kV4l2PixFmtMjpeg);
    assert(v4l2_pixel_format(sixcam_input_format("other")) ==
           kV4l2PixFmtMjpeg);
}
