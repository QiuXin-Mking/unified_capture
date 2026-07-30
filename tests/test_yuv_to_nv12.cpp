#include "hardware/video/yuv_to_nv12.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

void assert_visible_y_and_padding(const std::vector<uint8_t>& nv12) {
    const uint8_t expected[4][4] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16},
    };
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            assert(nv12[row * 8 + col] == expected[row][col]);
        }
        for (int col = 4; col < 8; ++col) {
            assert(nv12[row * 8 + col] == 0);
        }
    }
}

DecodedYuvFrame base_frame(const uint8_t* y) {
    DecodedYuvFrame frame;
    frame.width = 4;
    frame.height = 4;
    frame.y = {y, 4, 4, 4};
    return frame;
}

}  // namespace

int main() {
    const uint8_t y[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    std::vector<uint8_t> nv12;

    {
        const uint8_t u[] = {10, 20, 30, 40};
        const uint8_t v[] = {50, 60, 70, 80};
        DecodedYuvFrame frame = base_frame(y);
        frame.subsampling = YuvSubsampling::yuv420;
        frame.u = {u, 2, 2, 2};
        frame.v = {v, 2, 2, 2};
        assert(pack_yuv_to_nv12(frame, 8, nv12));
        assert(nv12.size() == 48);
        assert_visible_y_and_padding(nv12);
        const std::vector<uint8_t> expected_uv = {
            10, 50, 20, 60, 0, 0, 0, 0,
            30, 70, 40, 80, 0, 0, 0, 0,
        };
        assert(std::vector<uint8_t>(nv12.begin() + 32, nv12.end()) == expected_uv);
    }

    {
        const uint8_t u[] = {10, 20, 30, 40, 50, 60, 70, 80};
        const uint8_t v[] = {90, 100, 110, 120, 130, 140, 150, 160};
        DecodedYuvFrame frame = base_frame(y);
        frame.subsampling = YuvSubsampling::yuv422;
        frame.u = {u, 2, 4, 2};
        frame.v = {v, 2, 4, 2};
        assert(pack_yuv_to_nv12(frame, 8, nv12));
        const std::vector<uint8_t> expected_uv = {
            20, 100, 30, 110, 0, 0, 0, 0,
            60, 140, 70, 150, 0, 0, 0, 0,
        };
        assert(std::vector<uint8_t>(nv12.begin() + 32, nv12.end()) == expected_uv);
    }

    {
        const uint8_t u[] = {
            10, 20, 30, 40,
            50, 60, 70, 80,
            90, 100, 110, 120,
            130, 140, 150, 160,
        };
        const uint8_t v[] = {
            20, 30, 40, 50,
            60, 70, 80, 90,
            100, 110, 120, 130,
            140, 150, 160, 170,
        };
        DecodedYuvFrame frame = base_frame(y);
        frame.subsampling = YuvSubsampling::yuv444;
        frame.u = {u, 4, 4, 4};
        frame.v = {v, 4, 4, 4};
        assert(pack_yuv_to_nv12(frame, 8, nv12));
        const std::vector<uint8_t> expected_uv = {
            35, 45, 55, 65, 0, 0, 0, 0,
            115, 125, 135, 145, 0, 0, 0, 0,
        };
        assert(std::vector<uint8_t>(nv12.begin() + 32, nv12.end()) == expected_uv);
    }

    {
        DecodedYuvFrame frame = base_frame(y);
        frame.subsampling = YuvSubsampling::gray;
        assert(pack_yuv_to_nv12(frame, 8, nv12));
        const std::vector<uint8_t> expected_uv = {
            128, 128, 128, 128, 0, 0, 0, 0,
            128, 128, 128, 128, 0, 0, 0, 0,
        };
        assert(std::vector<uint8_t>(nv12.begin() + 32, nv12.end()) == expected_uv);
    }

    {
        DecodedYuvFrame odd = base_frame(y);
        odd.width = 3;
        assert(!pack_yuv_to_nv12(odd, 8, nv12));
    }
}
