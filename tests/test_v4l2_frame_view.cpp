#include "hardware/video/v4l2_frame_view.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    uint8_t source[] = {10, 20, 30, 40};
    V4l2FrameView view{source, sizeof(source), 123, 4567};
    CompressedFrame owned = copy_compressed_frame(view, 9, 9999);

    source[0] = 99;
    assert(owned.frame_idx == 9);
    assert(owned.pts_us == 4567);
    assert(owned.v4l2_sequence == 123);
    assert((owned.data == std::vector<uint8_t>{10, 20, 30, 40}));

    V4l2FrameView missing_timestamp{source, sizeof(source), 124, 0};
    owned = copy_compressed_frame(missing_timestamp, 10, 9999);
    assert(owned.pts_us == 9999);
    assert(owned.v4l2_sequence == 124);
}
