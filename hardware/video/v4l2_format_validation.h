#pragma once

#include <cerrno>
#include <cstdint>

template <typename Cleanup>
inline bool validate_v4l2_selected_format(
    uint32_t requested_format, int requested_width, int requested_height,
    uint32_t selected_format, int selected_width, int selected_height,
    Cleanup&& cleanup) {
    if (selected_format == requested_format &&
        selected_width == requested_width &&
        selected_height == requested_height) {
        return true;
    }

    constexpr int format_error = EINVAL;
    errno = format_error;
    cleanup();
    errno = format_error;
    return false;
}
