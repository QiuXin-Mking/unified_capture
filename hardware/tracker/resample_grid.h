#pragma once

#include <cstdint>
#include <vector>

inline std::vector<uint64_t> make_resample_grid(uint64_t first_timecode,
                                                 uint64_t last_timecode,
                                                 uint64_t interval) {
    std::vector<uint64_t> grid;
    if (interval == 0 || first_timecode > last_timecode) return grid;

    for (uint64_t timecode = first_timecode;; timecode += interval) {
        grid.push_back(timecode);
        if (last_timecode - timecode < interval) break;
    }
    return grid;
}
