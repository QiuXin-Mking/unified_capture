#include "hardware/cherry/cherry_h264_writer.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <future>
#include <string>
#include <thread>
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

void fill_pipe(int fd) {
    const std::vector<uint8_t> fill(4096, 0x5a);
    while (write(fd, fill.data(), fill.size()) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

void test_large_access_unit_completes_across_multiple_pipe_drains() {
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);
    const int flags = fcntl(pipe_fds[1], F_GETFL);
    assert(flags >= 0);
    assert(fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    FILE* fifo = fdopen(pipe_fds[1], "wb");
    FILE* metadata = tmpfile();
    assert(fifo != nullptr);
    assert(metadata != nullptr);
    std::string configure_error;
    assert(configure_cherry_fifo_stream(fifo, configure_error));

    std::atomic<size_t> drained_bytes{0};
    std::atomic<size_t> drain_calls{0};
    std::thread reader([&] {
        uint8_t bytes[4096];
        while (true) {
            const ssize_t count = read(pipe_fds[0], bytes, sizeof(bytes));
            if (count == 0) return;
            if (count > 0) {
                drained_bytes += static_cast<size_t>(count);
                ++drain_calls;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    CompressedFrame frame;
    frame.frame_idx = 1;
    frame.pts_us = 100;
    frame.v4l2_sequence = 2;
    frame.data.assign(512 * 1024, 0x6b);
    CherryH264Writer writer(
        fifo, metadata,
        {std::chrono::seconds(2)});
    assert(writer.process(frame) == VideoFrameProcessResult::ok);
    assert(writer.bytes() == frame.data.size());
    assert(fclose(fifo) == 0);
    reader.join();
    assert(drained_bytes.load() == frame.data.size());
    assert(drain_calls.load() > 2);
    assert(fclose(metadata) == 0);
    assert(close(pipe_fds[0]) == 0);
}

void test_normal_session_stop_does_not_cancel_started_access_unit() {
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);
    const int flags = fcntl(pipe_fds[1], F_GETFL);
    assert(flags >= 0);
    assert(fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK) == 0);

    FILE* fifo = fdopen(pipe_fds[1], "wb");
    FILE* metadata = tmpfile();
    assert(fifo != nullptr);
    assert(metadata != nullptr);
    std::string configure_error;
    assert(configure_cherry_fifo_stream(fifo, configure_error));
    assert(configure_error.empty());

    std::atomic<bool> acquisition_running{true};
    std::thread reader([&] {
        uint8_t bytes[4096];
        while (read(pipe_fds[0], bytes, sizeof(bytes)) > 0) {
            acquisition_running = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    CompressedFrame frame;
    frame.frame_idx = 1;
    frame.pts_us = 100;
    frame.v4l2_sequence = 2;
    frame.data.assign(256 * 1024, 0x4d);
    CherryH264Writer writer(fifo, metadata,
                            {std::chrono::seconds(2)});
    assert(writer.process(frame) == VideoFrameProcessResult::ok);
    assert(!acquisition_running);
    assert(writer.bytes() == frame.data.size());

    assert(fclose(fifo) == 0);
    reader.join();
    assert(fclose(metadata) == 0);
    assert(close(pipe_fds[0]) == 0);
}

void test_slow_drip_cannot_extend_absolute_deadline() {
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);
    const int flags = fcntl(pipe_fds[1], F_GETFL);
    assert(flags >= 0);
    assert(fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    FILE* fifo = fdopen(pipe_fds[1], "wb");
    FILE* metadata = tmpfile();
    assert(fifo != nullptr);
    assert(metadata != nullptr);
    std::string configure_error;
    assert(configure_cherry_fifo_stream(fifo, configure_error));

    std::atomic<bool> stop_reader{false};
    std::thread reader([&] {
        uint8_t bytes[1024];
        while (!stop_reader) {
            read(pipe_fds[0], bytes, sizeof(bytes));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    CompressedFrame frame;
    frame.data.assign(4 * 1024 * 1024, 0x7c);
    CherryH264Writer writer(fifo, metadata,
                            {std::chrono::milliseconds(40)});
    const auto begin = std::chrono::steady_clock::now();
    assert(writer.process(frame) == VideoFrameProcessResult::encoder_failure);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    assert(elapsed < std::chrono::milliseconds(200));
    assert(writer.bytes() < frame.data.size());
    assert(std::string(writer.error()).find("timed out") != std::string::npos);

    stop_reader = true;
    assert(fclose(fifo) == 0);
    reader.join();
    assert(fclose(metadata) == 0);
    assert(close(pipe_fds[0]) == 0);
}

void test_full_nonblocking_fifo_times_out_without_blocking() {
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);
    const int flags = fcntl(pipe_fds[1], F_GETFL);
    assert(flags >= 0);
    assert(fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    fill_pipe(pipe_fds[1]);

    FILE* fifo = fdopen(pipe_fds[1], "wb");
    FILE* metadata = tmpfile();
    assert(fifo != nullptr);
    assert(metadata != nullptr);
    std::string configure_error;
    assert(configure_cherry_fifo_stream(fifo, configure_error));

    const CompressedFrame frame{
        1, 100, 2, {0x00, 0x00, 0x00, 0x01, 0x65, 0xaa}};
    CherryH264Writer writer(
        fifo, metadata,
        {std::chrono::milliseconds(50)});
    auto result = std::async(std::launch::async, [&] {
        return writer.process(frame);
    });
    assert(result.wait_for(std::chrono::milliseconds(300)) ==
           std::future_status::ready);
    assert(result.get() == VideoFrameProcessResult::encoder_failure);
    assert(std::string(writer.error()).find("timed out") != std::string::npos);

    assert(fclose(fifo) == 0);
    assert(fclose(metadata) == 0);
    assert(close(pipe_fds[0]) == 0);
}

}  // namespace

int main() {
    test_large_access_unit_completes_across_multiple_pipe_drains();
    test_normal_session_stop_does_not_cancel_started_access_unit();
    test_slow_drip_cannot_extend_absolute_deadline();
    test_full_nonblocking_fifo_times_out_without_blocking();

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
