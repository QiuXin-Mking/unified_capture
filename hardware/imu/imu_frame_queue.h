#pragma once

#include "core/bounded_queue.h"

#include <array>
#include <cstdint>

struct ImuFrame {
    uint64_t frame_idx = 0;
    uint64_t pts_us = 0;
    uint32_t size = 0;
    std::array<uint8_t, 384> data{};
};

using ImuFrameQueue = BoundedQueue<ImuFrame>;
