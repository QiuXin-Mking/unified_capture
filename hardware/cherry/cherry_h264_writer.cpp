#include "hardware/cherry/cherry_h264_writer.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

CherryH264Writer::CherryH264Writer(FILE* video_file, FILE* metadata_file)
    : video_file_(video_file), metadata_file_(metadata_file) {}

VideoFrameProcessResult CherryH264Writer::process(
    const CompressedFrame& frame) {
    if (!error_.empty()) {
        return VideoFrameProcessResult::encoder_failure;
    }
    if (!video_file_ || !metadata_file_) {
        error_ = "Cherry H.264 output is not initialized";
        return VideoFrameProcessResult::encoder_failure;
    }
    if (frame.data.empty()) {
        error_ = "Cherry H.264 frame is empty";
        return VideoFrameProcessResult::encoder_failure;
    }

    errno = 0;
    const size_t video_written =
        fwrite(frame.data.data(), 1, frame.data.size(), video_file_);
    bytes_ += video_written;
    if (video_written != frame.data.size()) {
        set_file_error("Cherry H.264 write failed");
        return VideoFrameProcessResult::encoder_failure;
    }

    char metadata[128];
    const int metadata_size = snprintf(
        metadata, sizeof(metadata),
        "{\"v4l2_sequence\":%" PRIu32
        ",\"v4l2_timestamp_us\":%" PRIu64 "}\n",
        frame.v4l2_sequence, frame.pts_us);
    if (metadata_size < 0 ||
        static_cast<size_t>(metadata_size) >= sizeof(metadata)) {
        error_ = "Cherry H.264 metadata formatting failed";
        return VideoFrameProcessResult::encoder_failure;
    }

    errno = 0;
    const size_t metadata_written = fwrite(
        metadata, 1, static_cast<size_t>(metadata_size), metadata_file_);
    if (metadata_written != static_cast<size_t>(metadata_size)) {
        set_file_error("Cherry H.264 metadata write failed");
        return VideoFrameProcessResult::encoder_failure;
    }
    if (fflush(metadata_file_) != 0) {
        set_file_error("Cherry H.264 metadata flush failed");
        return VideoFrameProcessResult::encoder_failure;
    }
    return VideoFrameProcessResult::ok;
}

void CherryH264Writer::finish() {
    if (video_file_ && fflush(video_file_) != 0) {
        set_file_error("Cherry H.264 flush failed");
    }
    if (metadata_file_ && fflush(metadata_file_) != 0) {
        set_file_error("Cherry H.264 metadata flush failed");
    }
}

size_t CherryH264Writer::bytes() const {
    return bytes_;
}

const char* CherryH264Writer::error() const {
    return error_.c_str();
}

void CherryH264Writer::set_file_error(const char* operation) {
    if (!error_.empty()) {
        return;
    }
    error_ = operation;
    if (errno != 0) {
        error_ += ": ";
        error_ += strerror(errno);
    }
}
