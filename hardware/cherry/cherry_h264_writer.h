#pragma once

#include "hardware/video/capture_pipeline.h"

#include <cstddef>
#include <cstdio>
#include <string>

// The ffmpeg FIFO must stay nonblocking so a dead/non-reading child cannot
// trap the capture pipeline's processing thread in stdio buffering or write.
bool configure_cherry_fifo_stream(FILE* fifo, std::string& error);

class CherryH264Writer {
public:
    // Both FILE* streams are borrowed and must outlive this writer.
    CherryH264Writer(FILE* video_file, FILE* metadata_file);

    VideoFrameProcessResult process(const CompressedFrame& frame);
    void finish();

    size_t bytes() const;
    const char* error() const;

private:
    void set_file_error(const char* operation);

    FILE* video_file_ = nullptr;
    FILE* metadata_file_ = nullptr;
    size_t bytes_ = 0;
    std::string error_;
};
