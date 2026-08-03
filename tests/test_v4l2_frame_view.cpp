#include "hardware/video/v4l2_frame_view.h"
#include "hardware/video/v4l2_format_validation.h"

#include <cassert>
#include <cerrno>
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

    constexpr uint32_t h264_fourcc = 0x34363248;
    constexpr uint32_t mjpeg_fourcc = 0x47504a4d;
    bool cleanup_called = false;
    errno = ERANGE;
    assert(validate_v4l2_selected_format(
        h264_fourcc, 3200, 1200,
        h264_fourcc, 3200, 1200,
        [&] { cleanup_called = true; }));
    assert(!cleanup_called);
    assert(errno == ERANGE);

    const auto assert_mismatch = [&](uint32_t selected_format,
                                     int selected_width,
                                     int selected_height) {
        cleanup_called = false;
        errno = ERANGE;
        assert(!validate_v4l2_selected_format(
            h264_fourcc, 3200, 1200,
            selected_format, selected_width, selected_height,
            [&] {
                cleanup_called = true;
                errno = EIO;
            }));
        assert(cleanup_called);
        assert(errno == EINVAL);
    };
    assert_mismatch(mjpeg_fourcc, 3200, 1200);
    assert_mismatch(h264_fourcc, 1600, 1200);
    assert_mismatch(h264_fourcc, 3200, 600);
}
