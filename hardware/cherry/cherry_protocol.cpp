#include "hardware/cherry/cherry_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace cherry {
namespace {

constexpr uint8_t kResponseFlag = 0x01;
constexpr uint8_t kStreamMask = 0x07;
constexpr uint8_t kStartFlags = 0x01;
constexpr size_t kMaxImuSamples = 16;
constexpr size_t kMaxMagSamples = 16;
constexpr size_t kMaxFrameMetaSamples = 4;
constexpr size_t kImuMetaSize = 24;
constexpr size_t kImuSampleSize = 18;
constexpr size_t kMagMetaSize = 8;
constexpr size_t kMagSampleSize = 24;
constexpr size_t kFrameMetaHeaderSize = 8;
constexpr size_t kFrameMetaSampleSize = 16;
constexpr size_t kMaxBufferedBytes = 2 * (kHeaderSize + kMaxPayloadSize + kCrcSize);

uint16_t read_le16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t read_le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_le64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(read_le32(bytes)) |
           (static_cast<uint64_t>(read_le32(bytes + 4)) << 32);
}

int16_t read_le_i16(const uint8_t* bytes)
{
    const uint16_t raw = read_le16(bytes);
    int16_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

int32_t read_le_i32(const uint8_t* bytes)
{
    const uint32_t raw = read_le32(bytes);
    int32_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

void append_le16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

uint16_t crc16(std::span<const uint8_t> bytes)
{
    uint16_t crc = 0xffff;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? static_cast<uint16_t>((crc >> 1) ^ 0xa001) :
                                     static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

bool known_type(uint8_t type)
{
    switch (static_cast<MessageType>(type)) {
    case MessageType::start:
    case MessageType::stop:
    case MessageType::imu_data:
    case MessageType::mag_data:
    case MessageType::frame_meta:
    case MessageType::error:
        return true;
    }
    return false;
}

bool valid_error_detail(uint16_t detail)
{
    return detail <= 0x0008;
}

bool valid_imu_payload(std::span<const uint8_t> payload)
{
    if (payload.size() < kImuMetaSize) {
        return false;
    }
    const size_t gyro_count = read_le16(payload.data() + 20);
    const size_t acc_count = read_le16(payload.data() + 22);
    if (gyro_count > kMaxImuSamples || acc_count > kMaxImuSamples) {
        return false;
    }
    return payload.size() == kImuMetaSize + (gyro_count + acc_count) * kImuSampleSize;
}

bool valid_mag_payload(std::span<const uint8_t> payload)
{
    if (payload.size() < kMagMetaSize || payload[6] != 0 || payload[7] != 0) {
        return false;
    }
    const size_t count = read_le16(payload.data() + 4);
    if (count > kMaxMagSamples || payload.size() != kMagMetaSize + count * kMagSampleSize) {
        return false;
    }
    for (size_t offset = kMagMetaSize; offset < payload.size(); offset += kMagSampleSize) {
        if (payload[offset + 13] != 0 || payload[offset + 14] != 0 || payload[offset + 15] != 0) {
            return false;
        }
    }
    return true;
}

bool valid_frame_meta_payload(std::span<const uint8_t> payload)
{
    if (payload.size() < kFrameMetaHeaderSize || payload[6] != 0 || payload[7] != 0) {
        return false;
    }
    const size_t count = read_le16(payload.data() + 4);
    if (count == 0 || count > kMaxFrameMetaSamples ||
        payload.size() != kFrameMetaHeaderSize + count * kFrameMetaSampleSize) {
        return false;
    }
    for (size_t offset = kFrameMetaHeaderSize; offset < payload.size(); offset += kFrameMetaSampleSize) {
        if (payload[offset + 2] != 0 || payload[offset + 3] != 0) {
            return false;
        }
    }
    return true;
}

bool valid_frame_contract(MessageType type, uint8_t flags, uint16_t sequence,
                          std::span<const uint8_t> payload)
{
    if ((flags & ~kResponseFlag) != 0 || payload.size() > kMaxPayloadSize) {
        return false;
    }

    switch (type) {
    case MessageType::start:
        return sequence != 0 && payload.size() == 4 && payload[0] != 0 &&
               (payload[0] & ~kStreamMask) == 0 &&
               (payload[1] & ~kStartFlags) == 0 && payload[2] == 0 && payload[3] == 0;
    case MessageType::stop:
        return sequence != 0 && payload.empty();
    case MessageType::imu_data:
        return flags == 0 && sequence == 0 && valid_imu_payload(payload);
    case MessageType::mag_data:
        return flags == 0 && sequence == 0 && valid_mag_payload(payload);
    case MessageType::frame_meta:
        return flags == 0 && sequence == 0 && valid_frame_meta_payload(payload);
    case MessageType::error:
        return flags == kResponseFlag && sequence != 0 && payload.size() == 8 &&
               read_le16(payload.data() + 4) == sequence && valid_error_detail(read_le16(payload.data() + 6));
    }
    return false;
}

std::vector<uint8_t> encode_frame(MessageType type, uint8_t flags, uint16_t sequence,
                                  std::vector<uint8_t> payload)
{
    if (!valid_frame_contract(type, flags, sequence, payload)) {
        return {};
    }
    std::vector<uint8_t> frame{0x53, 0x59, kVersion, static_cast<uint8_t>(type), flags};
    append_le16(frame, sequence);
    append_le16(frame, static_cast<uint16_t>(payload.size()));
    frame.push_back(0);
    frame.insert(frame.end(), payload.begin(), payload.end());
    append_le16(frame, crc16(frame));
    return frame;
}

void discard_until_magic(std::vector<uint8_t>& buffer)
{
    static constexpr std::array<uint8_t, 2> kWireMagic{0x53, 0x59};
    const auto magic = std::search(buffer.begin(), buffer.end(), kWireMagic.begin(), kWireMagic.end());
    if (magic != buffer.end()) {
        buffer.erase(buffer.begin(), magic);
        return;
    }
    if (!buffer.empty() && buffer.back() == 0x53) {
        buffer.erase(buffer.begin(), buffer.end() - 1);
    } else {
        buffer.clear();
    }
}

void parse_available_frames(std::vector<uint8_t>& buffer, size_t& error_count,
                            std::vector<Frame>& frames)
{
    while (!buffer.empty()) {
        discard_until_magic(buffer);
        if (buffer.size() < 2 || buffer.size() < kHeaderSize) {
            return;
        }

        const uint16_t payload_size = read_le16(buffer.data() + 7);
        const uint8_t type = buffer[3];
        const uint8_t flags = buffer[4];
        const uint16_t sequence = read_le16(buffer.data() + 5);
        if (buffer[2] != kVersion || buffer[9] != 0 || !known_type(type) ||
            (flags & ~kResponseFlag) != 0 || payload_size > kMaxPayloadSize) {
            ++error_count;
            buffer.erase(buffer.begin());
            continue;
        }

        const size_t wire_size = kHeaderSize + payload_size + kCrcSize;
        if (buffer.size() < wire_size) {
            return;
        }
        if (read_le16(buffer.data() + kHeaderSize + payload_size) !=
            crc16(std::span<const uint8_t>(buffer.data(), kHeaderSize + payload_size))) {
            ++error_count;
            buffer.erase(buffer.begin());
            continue;
        }

        const auto payload = std::span<const uint8_t>(buffer.data() + kHeaderSize, payload_size);
        const auto message_type = static_cast<MessageType>(type);
        if (!valid_frame_contract(message_type, flags, sequence, payload)) {
            ++error_count;
            buffer.erase(buffer.begin());
            continue;
        }
        frames.push_back({message_type, flags, sequence, {payload.begin(), payload.end()}});
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(wire_size));
    }
}

} // namespace

std::vector<uint8_t> encode_start(uint16_t sequence, uint8_t stream_mask)
{
    return encode_frame(MessageType::start, 0, sequence, {stream_mask, 0, 0, 0});
}

std::vector<uint8_t> encode_stop(uint16_t sequence)
{
    return encode_frame(MessageType::stop, 0, sequence, {});
}

std::optional<ImuFrame> decode_imu(const Frame& frame)
{
    if (frame.type != MessageType::imu_data ||
        !valid_frame_contract(frame.type, frame.flags, frame.sequence, frame.payload)) {
        return std::nullopt;
    }
    const auto payload = std::span<const uint8_t>(frame.payload);
    ImuFrame decoded{read_le32(payload.data()), read_le64(payload.data() + 4),
                     read_le64(payload.data() + 12), {}, {}};
    const size_t gyro_count = read_le16(payload.data() + 20);
    const size_t acc_count = read_le16(payload.data() + 22);
    decoded.gyro.reserve(gyro_count);
    decoded.acc.reserve(acc_count);
    size_t offset = kImuMetaSize;
    const auto append_samples = [&payload, &offset](std::vector<ImuSample>& samples, size_t count) {
        for (size_t index = 0; index < count; ++index, offset += kImuSampleSize) {
            samples.push_back({read_le_i16(payload.data() + offset),
                               read_le_i16(payload.data() + offset + 2),
                               read_le_i16(payload.data() + offset + 4),
                               read_le_i32(payload.data() + offset + 6),
                               read_le64(payload.data() + offset + 10)});
        }
    };
    append_samples(decoded.gyro, gyro_count);
    append_samples(decoded.acc, acc_count);
    return decoded;
}

std::optional<MagFrame> decode_mag(const Frame& frame)
{
    if (frame.type != MessageType::mag_data ||
        !valid_frame_contract(frame.type, frame.flags, frame.sequence, frame.payload)) {
        return std::nullopt;
    }
    const auto payload = std::span<const uint8_t>(frame.payload);
    const size_t count = read_le16(payload.data() + 4);
    MagFrame decoded{read_le32(payload.data()), {}};
    decoded.samples.reserve(count);
    for (size_t offset = kMagMetaSize; offset < payload.size(); offset += kMagSampleSize) {
        decoded.samples.push_back({read_le_i32(payload.data() + offset),
                                   read_le_i32(payload.data() + offset + 4),
                                   read_le_i32(payload.data() + offset + 8),
                                   payload[offset + 12], read_le64(payload.data() + offset + 16)});
    }
    return decoded;
}

std::optional<FrameMeta> decode_frame_meta(const Frame& frame)
{
    if (frame.type != MessageType::frame_meta ||
        !valid_frame_contract(frame.type, frame.flags, frame.sequence, frame.payload)) {
        return std::nullopt;
    }
    const auto payload = std::span<const uint8_t>(frame.payload);
    const size_t count = read_le16(payload.data() + 4);
    FrameMeta decoded{read_le32(payload.data()), {}};
    decoded.samples.reserve(count);
    for (size_t offset = kFrameMetaHeaderSize; offset < payload.size(); offset += kFrameMetaSampleSize) {
        decoded.samples.push_back({payload[offset], payload[offset + 1], read_le32(payload.data() + offset + 4),
                                   read_le64(payload.data() + offset + 8)});
    }
    return decoded;
}

std::vector<Frame> StreamParser::push(std::span<const uint8_t> bytes)
{
    std::vector<Frame> frames;
    for (uint8_t byte : bytes) {
        if (buffer_.size() == kMaxBufferedBytes) {
            parse_available_frames(buffer_, error_count_, frames);
            if (buffer_.size() == kMaxBufferedBytes) {
                ++error_count_;
                buffer_.erase(buffer_.begin());
            }
        }
        buffer_.push_back(byte);
        parse_available_frames(buffer_, error_count_, frames);
    }
    return frames;
}

size_t StreamParser::error_count() const
{
    return error_count_;
}

size_t StreamParser::retained_buffer_size() const
{
    return buffer_.size();
}

} // namespace cherry
