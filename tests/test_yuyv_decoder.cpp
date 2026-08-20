#include "hardware/video/yuyv_decoder.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    // Two pixels per packed YUYV macropixel: Y0 U Y1 V.
    const std::vector<uint8_t> packed = {
        10, 100, 20, 150,
        30, 110, 40, 160,
        50, 120, 60, 170,
        70, 130, 80, 180,
    };

    YuyvDecoder decoder;
    DecodedYuvFrame decoded;
    assert(decoder.decode(packed.data(), packed.size(), 4, 2, decoded));
    assert(decoded.width == 4);
    assert(decoded.height == 2);
    assert(decoded.subsampling == YuvSubsampling::yuv422);
    assert(decoded.y.width == 4);
    assert(decoded.y.height == 2);
    assert(decoded.y.stride == 4);
    assert(decoded.u.width == 2);
    assert(decoded.u.height == 2);
    assert(decoded.v.width == 2);
    assert(decoded.v.height == 2);

    assert((std::vector<uint8_t>(decoded.y.data, decoded.y.data + 8) ==
            std::vector<uint8_t>{10, 20, 30, 40, 50, 60, 70, 80}));
    assert((std::vector<uint8_t>(decoded.u.data, decoded.u.data + 4) ==
            std::vector<uint8_t>{100, 110, 120, 130}));
    assert((std::vector<uint8_t>(decoded.v.data, decoded.v.data + 4) ==
            std::vector<uint8_t>{150, 160, 170, 180}));

    assert(!decoder.decode(packed.data(), packed.size() - 1, 4, 2, decoded));
    assert(!decoder.decode(packed.data(), packed.size(), 3, 2, decoded));
}
