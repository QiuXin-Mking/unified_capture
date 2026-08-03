#pragma once

#include "hardware/video/capture_pipeline.h"

#include <cstddef>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

// The ffmpeg FIFO must stay nonblocking so a dead/non-reading child cannot
// trap the capture pipeline's processing thread in stdio buffering or write.
bool configure_cherry_fifo_stream(FILE* fifo, std::string& error);

struct CherryH264WriteOptions {
    std::chrono::milliseconds timeout{500};
    std::function<bool()> keep_running;
};

class CherryH264Writer {
public:
    // Both FILE* streams are borrowed and must outlive this writer.
    CherryH264Writer(FILE* video_file, FILE* metadata_file,
                     CherryH264WriteOptions options = {});

    VideoFrameProcessResult process(const CompressedFrame& frame);
    void finish();

    size_t bytes() const;
    const char* error() const;

private:
    void set_file_error(const char* operation);
    bool write_video(const CompressedFrame& frame);
    bool write_nonblocking(const uint8_t* data, size_t size, int fd);

    FILE* video_file_ = nullptr;
    FILE* metadata_file_ = nullptr;
    size_t bytes_ = 0;
    std::string error_;
    CherryH264WriteOptions options_;
};
