#include "hardware/imu/imu_decode.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

constexpr int kEncodedPixels = 8 + 16 * 8 * IMU_USIZE + 8;

void encode_line(uint8_t* line, int length,
                 const std::array<uint8_t, 16>& payload) {
    std::fill(line, line + length, 255);
    line[4] = 0;
    int sample = 8;
    for (uint8_t byte : payload) {
        for (int bit = 0; bit < 8; ++bit) {
            const uint8_t value = ((byte >> bit) & 1U) ? 100 : 0;
            line[sample] = value;
            line[sample + 1] = value;
            sample += IMU_USIZE;
        }
    }
}

}  // namespace

int main() {
    const std::array<uint8_t, 16> payload = {
        0x2b, 0x01, 0x7e, 0x80, 0x55, 0xaa, 0x11, 0x22,
        0x33, 0x44, 0x66, 0x77, 0x88, 0x99, 0xbb, 0xcc,
    };
    std::array<uint8_t, 384> decoded{};

    {
        const int width = kEncodedPixels;
        const int height = 17 * IMU_USIZE;
        const int stride = width + 13;
        std::vector<uint8_t> y(static_cast<size_t>(stride) * height, 0);
        for (int row = 3; row < height; row += IMU_USIZE) {
            encode_line(y.data() + row * stride, width, payload);
        }
        const uint32_t size = imu_read_luma_horizontal(
            y.data(), width, height, stride, decoded.data());
        assert(size == IMU_TARGET);
        for (int group = 0; group < 17; ++group) {
            assert(std::equal(payload.begin(), payload.end(),
                              decoded.begin() + group * IMU_GROUP));
        }
    }

    {
        const int width = 17 * IMU_USIZE;
        const int height = kEncodedPixels;
        const int stride = width + 7;
        std::vector<uint8_t> y(static_cast<size_t>(stride) * height, 0);
        std::vector<uint8_t> line(height);
        encode_line(line.data(), height, payload);
        for (int col = 3; col < width; col += IMU_USIZE) {
            for (int row = 0; row < height; ++row) {
                y[static_cast<size_t>(row) * stride + col] = line[row];
            }
        }
        const uint32_t size = imu_read_luma_vertical(
            y.data(), width, height, stride, decoded.data());
        assert(size == IMU_TARGET);
        for (int group = 0; group < 17; ++group) {
            assert(std::equal(payload.begin(), payload.end(),
                              decoded.begin() + group * IMU_GROUP));
        }
    }
}
