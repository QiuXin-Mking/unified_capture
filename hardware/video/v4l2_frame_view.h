#pragma once

#include "hardware/video/compressed_frame_queue.h"

#include <cstddef>
#include <cstdint>

struct V4l2FrameView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t sequence = 0;
    uint64_t timestamp_us = 0;
};

inline CompressedFrame copy_compressed_frame(const V4l2FrameView& view,
                                             uint64_t frame_idx,
                                             uint64_t fallback_pts_us) {
    CompressedFrame frame;
    frame.frame_idx = frame_idx;
    frame.pts_us = view.timestamp_us != 0 ? view.timestamp_us : fallback_pts_us;
    frame.v4l2_sequence = view.sequence;
    if (view.data && view.size > 0) {
        frame.data.assign(view.data, view.data + view.size);
    }
    return frame;
}
