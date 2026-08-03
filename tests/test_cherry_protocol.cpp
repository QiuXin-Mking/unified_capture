#include "hardware/cherry/cherry_protocol.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace {

void append_le16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void append_le32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void append_le64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
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

std::vector<uint8_t> make_frame(cherry::MessageType type, uint8_t flags,
                                uint16_t sequence, std::vector<uint8_t> payload)
{
    std::vector<uint8_t> frame{0x53, 0x59, 0x03, static_cast<uint8_t>(type), flags};
    append_le16(frame, sequence);
    append_le16(frame, static_cast<uint16_t>(payload.size()));
    frame.push_back(0x00);
    frame.insert(frame.end(), payload.begin(), payload.end());
    append_le16(frame, crc16(frame));
    return frame;
}

std::vector<uint8_t> make_max_imu_frame()
{
    std::vector<uint8_t> payload(600, 0);
    payload[20] = 16;
    payload[22] = 16;
    return make_frame(cherry::MessageType::imu_data, 0, 0, std::move(payload));
}

void test_control_encoding_and_streaming_parse()
{
    const std::vector<uint8_t> expected_start{
        0x53,0x59,0x03,0x01,0x00,0x01,0x00,0x04,
        0x00,0x00,0x07,0x00,0x00,0x00,0x6c,0x17};
    const std::vector<uint8_t> expected_stop{
        0x53,0x59,0x03,0x02,0x00,0x02,
        0x00,0x00,0x00,0x00,0x0f,0x0f};

    assert(cherry::encode_start(1, 0x07) == expected_start);
    assert(cherry::encode_stop(2) == expected_stop);

    cherry::StreamParser parser;
    std::vector<cherry::Frame> frames;
    for (uint8_t byte : expected_start) {
        const auto emitted = parser.push(std::span<const uint8_t>(&byte, 1));
        frames.insert(frames.end(), emitted.begin(), emitted.end());
    }
    assert(frames.size() == 1);
    assert(frames[0].type == cherry::MessageType::start);
    assert(frames[0].flags == 0);
    assert(frames[0].sequence == 1);
    assert(frames[0].payload == std::vector<uint8_t>({0x07, 0x00, 0x00, 0x00}));
    assert(parser.error_count() == 0);

    std::vector<uint8_t> corrupted = expected_start;
    corrupted.back() ^= 0x01;
    const auto valid_stop_response = make_frame(cherry::MessageType::stop, 0x01, 2, {});
    std::vector<uint8_t> mixed{0x12, 0x53, 0x00, 0xff};
    mixed.insert(mixed.end(), corrupted.begin(), corrupted.end());
    mixed.insert(mixed.end(), valid_stop_response.begin(), valid_stop_response.end());
    const auto recovered = parser.push(mixed);
    assert(recovered.size() == 1);
    assert(recovered[0].type == cherry::MessageType::stop);
    assert(recovered[0].flags == 0x01);
    assert(recovered[0].sequence == 2);
    assert(recovered[0].payload.empty());
    assert(parser.error_count() == 1);
}

void test_parser_consumes_arbitrarily_large_pushes_without_dropping_frames()
{
    const auto max_imu_frame = make_max_imu_frame();
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), max_imu_frame.begin(), max_imu_frame.end());
    combined.insert(combined.end(), max_imu_frame.begin(), max_imu_frame.end());
    combined.insert(combined.end(), max_imu_frame.begin(), max_imu_frame.end());

    cherry::StreamParser parser;
    const auto frames = parser.push(combined);
    assert(frames.size() == 3);
    for (const auto& frame : frames) {
        assert(frame.type == cherry::MessageType::imu_data);
        assert(frame.payload.size() == cherry::kMaxPayloadSize);
    }
    assert(parser.error_count() == 0);
    assert(parser.retained_buffer_size() == 0);

    std::vector<uint8_t> garbage(8192, 0x55);
    const auto discarded = parser.push(garbage);
    assert(discarded.empty());
    assert(parser.retained_buffer_size() == 0);
    const auto recovered = parser.push(cherry::encode_start(1, 0x07));
    assert(recovered.size() == 1);
    assert(recovered[0].type == cherry::MessageType::start);
    assert(parser.retained_buffer_size() == 0);
}

void test_parser_rejects_zero_start_mask_and_zero_frame_meta_count()
{
    assert(cherry::encode_start(1, 0).empty());

    const auto valid_stop_response = make_frame(cherry::MessageType::stop, 0x01, 2, {});
    const auto zero_mask_start = make_frame(cherry::MessageType::start, 0, 1, {0, 0, 0, 0});
    cherry::StreamParser start_parser;
    std::vector<uint8_t> start_then_stop = zero_mask_start;
    start_then_stop.insert(start_then_stop.end(), valid_stop_response.begin(), valid_stop_response.end());
    const auto start_recovered = start_parser.push(start_then_stop);
    assert(start_recovered.size() == 1);
    assert(start_recovered[0].type == cherry::MessageType::stop);
    assert(start_parser.error_count() == 1);

    const std::vector<uint8_t> zero_meta_payload{0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0};
    const cherry::Frame zero_meta{cherry::MessageType::frame_meta, 0, 0, zero_meta_payload};
    assert(!cherry::decode_frame_meta(zero_meta).has_value());

    cherry::StreamParser meta_parser;
    std::vector<uint8_t> meta_then_stop =
        make_frame(cherry::MessageType::frame_meta, 0, 0, zero_meta_payload);
    meta_then_stop.insert(meta_then_stop.end(), valid_stop_response.begin(), valid_stop_response.end());
    const auto meta_recovered = meta_parser.push(meta_then_stop);
    assert(meta_recovered.size() == 1);
    assert(meta_recovered[0].type == cherry::MessageType::stop);
    assert(meta_parser.error_count() == 1);
}

void test_typed_little_endian_decoders()
{
    std::vector<uint8_t> imu_payload;
    append_le32(imu_payload, 0x11223344);
    append_le64(imu_payload, 0x0123456789abcdefULL);
    append_le64(imu_payload, 0xfedcba9876543210ULL);
    append_le16(imu_payload, 1);
    append_le16(imu_payload, 1);
    append_le16(imu_payload, 0xffff);
    append_le16(imu_payload, 0x8000);
    append_le16(imu_payload, 12345);
    append_le32(imu_payload, 0xf8a432eb);
    append_le64(imu_payload, 0x1020304050607080ULL);
    append_le16(imu_payload, 0xfffe);
    append_le16(imu_payload, 2);
    append_le16(imu_payload, 0xfffd);
    append_le32(imu_payload, 0xffffffd6);
    append_le64(imu_payload, 0x8877665544332211ULL);
    const cherry::Frame imu_frame{cherry::MessageType::imu_data, 0, 0, imu_payload};
    const auto imu = cherry::decode_imu(imu_frame);
    assert(imu.has_value());
    assert(imu->generation == 0x11223344);
    assert(imu->window_begin_pts_us == 0x0123456789abcdefULL);
    assert(imu->window_end_pts_us == 0xfedcba9876543210ULL);
    assert(imu->gyro.size() == 1);
    assert(imu->acc.size() == 1);
    assert(imu->gyro[0].x == -1);
    assert(imu->gyro[0].y == -32768);
    assert(imu->gyro[0].z == 12345);
    assert(imu->gyro[0].temperature == -123456789);
    assert(imu->gyro[0].pts_us == 0x1020304050607080ULL);
    assert(imu->acc[0].x == -2);
    assert(imu->acc[0].y == 2);
    assert(imu->acc[0].z == -3);
    assert(imu->acc[0].temperature == -42);
    assert(imu->acc[0].pts_us == 0x8877665544332211ULL);

    std::vector<uint8_t> mag_payload;
    append_le32(mag_payload, 0x55667788);
    append_le16(mag_payload, 1);
    append_le16(mag_payload, 0);
    append_le32(mag_payload, 0xffffffff);
    append_le32(mag_payload, 0xfffcf2c0);
    append_le32(mag_payload, 123456);
    mag_payload.push_back(0x9a);
    mag_payload.insert(mag_payload.end(), {0, 0, 0});
    append_le64(mag_payload, 0x1029384756abcdefULL);
    const cherry::Frame mag_frame{cherry::MessageType::mag_data, 0, 0, mag_payload};
    const auto mag = cherry::decode_mag(mag_frame);
    assert(mag.has_value());
    assert(mag->generation == 0x55667788);
    assert(mag->samples.size() == 1);
    assert(mag->samples[0].x_raw == -1);
    assert(mag->samples[0].y_raw == -200000);
    assert(mag->samples[0].z_raw == 123456);
    assert(mag->samples[0].tout_raw == 0x9a);
    assert(mag->samples[0].pts_us == 0x1029384756abcdefULL);

    std::vector<uint8_t> meta_payload;
    append_le32(meta_payload, 0xaabbccdd);
    append_le16(meta_payload, 2);
    append_le16(meta_payload, 0);
    meta_payload.insert(meta_payload.end(), {0, 7, 0, 0});
    append_le32(meta_payload, 0x10203040);
    append_le64(meta_payload, 0x0123456789abcdefULL);
    meta_payload.insert(meta_payload.end(), {1, 8, 0, 0});
    append_le32(meta_payload, 0x55667788);
    append_le64(meta_payload, 0xfedcba9876543210ULL);
    const cherry::Frame meta_frame{cherry::MessageType::frame_meta, 0, 0, meta_payload};
    const auto meta = cherry::decode_frame_meta(meta_frame);
    assert(meta.has_value());
    assert(meta->generation == 0xaabbccdd);
    assert(meta->samples.size() == 2);
    assert(meta->samples[0].sensor_idx == 0);
    assert(meta->samples[0].vi_pipe == 7);
    assert(meta->samples[0].frame_id == 0x10203040);
    assert(meta->samples[0].frame_pts_us == 0x0123456789abcdefULL);
    assert(meta->samples[1].sensor_idx == 1);
    assert(meta->samples[1].vi_pipe == 8);
    assert(meta->samples[1].frame_id == 0x55667788);
    assert(meta->samples[1].frame_pts_us == 0xfedcba9876543210ULL);
}

void test_decoders_reject_malformed_counts_and_reserved_bytes()
{
    const cherry::Frame malformed_imu{
        cherry::MessageType::imu_data, 0, 0,
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 17, 0, 0, 0}};
    assert(!cherry::decode_imu(malformed_imu).has_value());

    const cherry::Frame malformed_mag{
        cherry::MessageType::mag_data, 0, 0,
        {0, 0, 0, 0, 0, 0, 1, 0}};
    assert(!cherry::decode_mag(malformed_mag).has_value());

    const cherry::Frame malformed_meta{
        cherry::MessageType::frame_meta, 0, 0,
        {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0}};
    assert(!cherry::decode_frame_meta(malformed_meta).has_value());
}

} // namespace

int main()
{
    test_control_encoding_and_streaming_parse();
    test_parser_consumes_arbitrarily_large_pushes_without_dropping_frames();
    test_parser_rejects_zero_start_mask_and_zero_frame_meta_count();
    test_typed_little_endian_decoders();
    test_decoders_reject_malformed_counts_and_reserved_bytes();
    return 0;
}
