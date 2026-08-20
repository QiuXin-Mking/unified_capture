#pragma once

#include <cstdint>
#include <cstring>

// Keep this policy header portable so its unit tests build on macOS too.
constexpr uint32_t kV4l2PixFmtMjpeg =
    static_cast<uint32_t>('M') |
    (static_cast<uint32_t>('J') << 8) |
    (static_cast<uint32_t>('P') << 16) |
    (static_cast<uint32_t>('G') << 24);
constexpr uint32_t kV4l2PixFmtYuyv =
    static_cast<uint32_t>('Y') |
    (static_cast<uint32_t>('U') << 8) |
    (static_cast<uint32_t>('Y') << 16) |
    (static_cast<uint32_t>('V') << 24);

enum class VideoInputFormat {
    mjpeg,
    yuyv,
};

inline VideoInputFormat sixcam_input_format(const char* camera_name) {
    return camera_name && std::strcmp(camera_name, "jhh04") == 0
               ? VideoInputFormat::yuyv
               : VideoInputFormat::mjpeg;
}

inline uint32_t v4l2_pixel_format(VideoInputFormat format) {
    return format == VideoInputFormat::yuyv
               ? kV4l2PixFmtYuyv
               : kV4l2PixFmtMjpeg;
}
