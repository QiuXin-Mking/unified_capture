#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

enum class YuvSubsampling {
    yuv420,
    yuv422,
    yuv444,
    gray,
};

struct YuvPlaneView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

struct DecodedYuvFrame {
    int width = 0;
    int height = 0;
    YuvSubsampling subsampling = YuvSubsampling::gray;
    YuvPlaneView y;
    YuvPlaneView u;
    YuvPlaneView v;
};

namespace yuv_to_nv12_detail {

inline bool plane_valid(const YuvPlaneView& plane, int min_width,
                        int min_height) {
    return plane.data && plane.width >= min_width &&
           plane.height >= min_height && plane.stride >= plane.width;
}

inline uint8_t mean2(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>((static_cast<unsigned int>(a) + b) / 2);
}

inline uint8_t mean4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return static_cast<uint8_t>(
        (static_cast<unsigned int>(a) + b + c + d) / 4);
}

}  // namespace yuv_to_nv12_detail

inline bool pack_yuv_to_nv12(const DecodedYuvFrame& input, int nv12_stride,
                             std::vector<uint8_t>& nv12) {
    const int width = input.width;
    const int height = input.height;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        nv12_stride < width ||
        !yuv_to_nv12_detail::plane_valid(input.y, width, height)) {
        return false;
    }

    const int chroma_width = width / 2;
    const int chroma_height = height / 2;
    if (input.subsampling == YuvSubsampling::yuv420 &&
        (!yuv_to_nv12_detail::plane_valid(
             input.u, chroma_width, chroma_height) ||
         !yuv_to_nv12_detail::plane_valid(
             input.v, chroma_width, chroma_height))) {
        return false;
    }
    if (input.subsampling == YuvSubsampling::yuv422 &&
        (!yuv_to_nv12_detail::plane_valid(input.u, chroma_width, height) ||
         !yuv_to_nv12_detail::plane_valid(input.v, chroma_width, height))) {
        return false;
    }
    if (input.subsampling == YuvSubsampling::yuv444 &&
        (!yuv_to_nv12_detail::plane_valid(input.u, width, height) ||
         !yuv_to_nv12_detail::plane_valid(input.v, width, height))) {
        return false;
    }

    nv12.assign(static_cast<size_t>(nv12_stride) * height * 3 / 2, 0);
    for (int row = 0; row < height; ++row) {
        std::memcpy(nv12.data() + static_cast<size_t>(row) * nv12_stride,
                    input.y.data + static_cast<size_t>(row) * input.y.stride,
                    width);
    }

    uint8_t* uv = nv12.data() + static_cast<size_t>(nv12_stride) * height;
    for (int row = 0; row < chroma_height; ++row) {
        for (int col = 0; col < chroma_width; ++col) {
            uint8_t u = 128;
            uint8_t v = 128;
            if (input.subsampling == YuvSubsampling::yuv420) {
                u = input.u.data[static_cast<size_t>(row) * input.u.stride + col];
                v = input.v.data[static_cast<size_t>(row) * input.v.stride + col];
            } else if (input.subsampling == YuvSubsampling::yuv422) {
                const size_t u0 = static_cast<size_t>(row * 2) * input.u.stride + col;
                const size_t v0 = static_cast<size_t>(row * 2) * input.v.stride + col;
                u = yuv_to_nv12_detail::mean2(
                    input.u.data[u0], input.u.data[u0 + input.u.stride]);
                v = yuv_to_nv12_detail::mean2(
                    input.v.data[v0], input.v.data[v0 + input.v.stride]);
            } else if (input.subsampling == YuvSubsampling::yuv444) {
                const size_t u0 =
                    static_cast<size_t>(row * 2) * input.u.stride + col * 2;
                const size_t v0 =
                    static_cast<size_t>(row * 2) * input.v.stride + col * 2;
                u = yuv_to_nv12_detail::mean4(
                    input.u.data[u0], input.u.data[u0 + 1],
                    input.u.data[u0 + input.u.stride],
                    input.u.data[u0 + input.u.stride + 1]);
                v = yuv_to_nv12_detail::mean4(
                    input.v.data[v0], input.v.data[v0 + 1],
                    input.v.data[v0 + input.v.stride],
                    input.v.data[v0 + input.v.stride + 1]);
            }
            const size_t offset =
                static_cast<size_t>(row) * nv12_stride + col * 2;
            uv[offset] = u;
            uv[offset + 1] = v;
        }
    }
    return true;
}
