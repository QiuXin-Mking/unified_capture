#include "hardware/cherry/cherry_h264_writer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::vector<uint8_t> read_bytes(FILE* file) {
    assert(fflush(file) == 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    std::vector<uint8_t> bytes;
    uint8_t buffer[64];
    while (true) {
        const size_t count = fread(buffer, 1, sizeof(buffer), file);
        bytes.insert(bytes.end(), buffer, buffer + count);
        if (count < sizeof(buffer)) {
            assert(feof(file));
            break;
        }
    }
    return bytes;
}

std::string read_text(FILE* file) {
    const std::vector<uint8_t> bytes = read_bytes(file);
    return std::string(bytes.begin(), bytes.end());
}

void assert_metadata_line(const std::string& line) {
    unsigned int sequence = 0;
    unsigned long long timestamp_us = 0;
    int consumed = 0;
    const int fields = std::sscanf(
        line.c_str(),
        "{\"v4l2_sequence\":%u,\"v4l2_timestamp_us\":%llu}%n",
        &sequence, &timestamp_us, &consumed);
    assert(fields == 2);
    assert(consumed == static_cast<int>(line.size() - 1));
    assert(line.back() == '\n');
    assert(sequence == 42);
    assert(timestamp_us == 123456789ULL);
}

}  // namespace

int main() {
    const CompressedFrame frame{
        7,
        123456789,
        42,
        {0x00, 0x00, 0x00, 0x01, 0x65, 0xaa},
    };

    char metadata_path[] = "/tmp/cherry-h264-metadata-XXXXXX";
    const int metadata_fd = mkstemp(metadata_path);
    assert(metadata_fd >= 0);

    FILE* video = tmpfile();
    FILE* metadata = fdopen(metadata_fd, "w+b");
    assert(video != nullptr);
    assert(metadata != nullptr);

    {
        CherryH264Writer writer(video, metadata);
        assert(writer.process(frame) == VideoFrameProcessResult::ok);
        FILE* metadata_reader = fopen(metadata_path, "rb");
        assert(metadata_reader != nullptr);
        assert_metadata_line(read_text(metadata_reader));
        assert(fclose(metadata_reader) == 0);

        writer.finish();
        assert(std::string(writer.error()).empty());
        assert(writer.bytes() == 6);
        assert(read_bytes(video) == frame.data);
    }

    assert(fclose(video) == 0);
    assert(fclose(metadata) == 0);
    assert(unlink(metadata_path) == 0);

    FILE* empty_video = tmpfile();
    FILE* empty_metadata = tmpfile();
    assert(empty_video != nullptr);
    assert(empty_metadata != nullptr);

    {
        CherryH264Writer empty_writer(empty_video, empty_metadata);
        const CompressedFrame empty_frame{8, 123456790, 43, {}};
        assert(empty_writer.process(empty_frame) ==
               VideoFrameProcessResult::encoder_failure);
        assert(std::string(empty_writer.error()).size() > 0);
        assert(empty_writer.bytes() == 0);
        assert(read_bytes(empty_video).empty());
        assert(read_text(empty_metadata).empty());
    }

    assert(fclose(empty_video) == 0);
    assert(fclose(empty_metadata) == 0);

    char read_only_path[] = "/tmp/cherry-h264-writer-XXXXXX";
    const int fd = mkstemp(read_only_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    FILE* read_only_video = fopen(read_only_path, "rb");
    FILE* failure_metadata = tmpfile();
    assert(read_only_video != nullptr);
    assert(failure_metadata != nullptr);

    {
        CherryH264Writer failing_writer(read_only_video, failure_metadata);
        assert(failing_writer.process(frame) ==
               VideoFrameProcessResult::encoder_failure);
        assert(std::string(failing_writer.error()).size() > 0);
        assert(failing_writer.bytes() == 0);
    }

    assert(fclose(read_only_video) == 0);
    assert(fclose(failure_metadata) == 0);
    assert(unlink(read_only_path) == 0);
}
