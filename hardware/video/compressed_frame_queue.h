#pragma once

#include "core/bounded_queue.h"

#include <cstdint>
#include <vector>

struct CompressedFrame {
    uint64_t frame_idx = 0;
    uint64_t pts_us = 0;
    uint32_t v4l2_sequence = 0;
    std::vector<uint8_t> data;
};

using CompressedFrameQueue = BoundedQueue<CompressedFrame>;
