#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cherry {

constexpr uint16_t kMagic = 0x5953;
constexpr uint8_t kVersion = 3;
constexpr size_t kHeaderSize = 10;
constexpr size_t kCrcSize = 2;
constexpr size_t kMaxPayloadSize = 600;

enum class MessageType : uint8_t {
    start = 0x01,
    stop = 0x02,
    imu_data = 0x04,
    mag_data = 0x05,
    frame_meta = 0x09,
    error = 0x0a,
};

struct Frame {
    MessageType type;
    uint8_t flags;
    uint16_t sequence;
    std::vector<uint8_t> payload;
};

struct ImuSample {
    int16_t x;
    int16_t y;
    int16_t z;
    int32_t temperature;
    uint64_t pts_us;
};

struct ImuFrame {
    uint32_t generation;
    uint64_t window_begin_pts_us;
    uint64_t window_end_pts_us;
    std::vector<ImuSample> gyro;
    std::vector<ImuSample> acc;
};

struct MagSample {
    int32_t x_raw;
    int32_t y_raw;
    int32_t z_raw;
    uint8_t tout_raw;
    uint64_t pts_us;
};

struct MagFrame {
    uint32_t generation;
    std::vector<MagSample> samples;
};

struct FrameMetaSample {
    uint8_t sensor_idx;
    uint8_t vi_pipe;
    uint32_t frame_id;
    uint64_t frame_pts_us;
};

struct FrameMeta {
    uint32_t generation;
    std::vector<FrameMetaSample> samples;
};

std::vector<uint8_t> encode_start(uint16_t sequence, uint8_t stream_mask);
std::vector<uint8_t> encode_stop(uint16_t sequence);

std::optional<ImuFrame> decode_imu(const Frame& frame);
std::optional<MagFrame> decode_mag(const Frame& frame);
std::optional<FrameMeta> decode_frame_meta(const Frame& frame);

class StreamParser {
public:
    std::vector<Frame> push(std::span<const uint8_t> bytes);
    size_t error_count() const;

private:
    std::vector<uint8_t> buffer_;
    size_t error_count_ = 0;
};

} // namespace cherry
