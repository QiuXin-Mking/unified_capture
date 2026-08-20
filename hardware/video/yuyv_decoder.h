#pragma once

#include "hardware/video/yuv_to_nv12.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class YuyvDecoder {
public:
    bool decode(const uint8_t* packed, size_t size, int width, int height,
                DecodedYuvFrame& output) {
        if (!packed || width <= 0 || height <= 0 || (width & 1) ||
            size < static_cast<size_t>(width) * height * 2) {
            error_ = "invalid YUYV frame";
            return false;
        }

        const size_t pixel_count = static_cast<size_t>(width) * height;
        y_.resize(pixel_count);
        u_.resize(pixel_count / 2);
        v_.resize(pixel_count / 2);
        for (int row = 0; row < height; ++row) {
            const uint8_t* source = packed +
                static_cast<size_t>(row) * width * 2;
            uint8_t* y = y_.data() + static_cast<size_t>(row) * width;
            uint8_t* u = u_.data() + static_cast<size_t>(row) * width / 2;
            uint8_t* v = v_.data() + static_cast<size_t>(row) * width / 2;
            for (int col = 0; col < width; col += 2) {
                y[col] = source[0];
                u[col / 2] = source[1];
                y[col + 1] = source[2];
                v[col / 2] = source[3];
                source += 4;
            }
        }

        output.width = width;
        output.height = height;
        output.subsampling = YuvSubsampling::yuv422;
        output.y = {y_.data(), width, height, width};
        output.u = {u_.data(), width / 2, height, width / 2};
        output.v = {v_.data(), width / 2, height, width / 2};
        error_.clear();
        return true;
    }

    const char* error() const { return error_.c_str(); }

private:
    std::vector<uint8_t> y_;
    std::vector<uint8_t> u_;
    std::vector<uint8_t> v_;
    std::string error_;
};
