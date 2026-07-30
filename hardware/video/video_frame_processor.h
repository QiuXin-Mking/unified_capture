#pragma once

#include "core/camera_config.h"
#include "hardware/imu/imu_decode.h"
#include "hardware/imu/imu_frame_queue.h"
#include "hardware/video/capture_control.h"
#include "hardware/video/capture_pipeline.h"
#include "hardware/video/mjpeg_yuv_decoder.h"
#include "hardware/video/mpp_encoder.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct VideoFrameProcessorOptions {
    const char* camera_name = "camera";
    bool output_h265 = false;
    bool has_imu = false;
    ImuOrientation imu_orientation = ImuOrientation::HORIZONTAL_TOP;
    int expected_width = 0;
    int expected_height = 0;
    int nv12_stride = 0;
};

struct VideoFrameProcessorTimings {
    uint64_t decode_us = 0;
    uint64_t imu_us = 0;
    uint64_t nv12_us = 0;
    uint64_t encoder_us = 0;
    uint64_t frames = 0;
};

class VideoFrameProcessor {
public:
    VideoFrameProcessor(VideoFrameProcessorOptions options,
                        MppEncoder* encoder,
                        FILE* h265_fp,
                        ImuFrameQueue* imu_queue,
                        VideoCaptureControl* control)
        : options_(std::move(options))
        , encoder_(encoder)
        , h265_fp_(h265_fp)
        , imu_queue_(imu_queue)
        , control_(control) {}

    VideoFrameProcessResult process(const CompressedFrame& frame) {
        const auto decode_start = Clock::now();
        DecodedYuvFrame decoded;
        if (!decoder_.decode(frame.data.data(), frame.data.size(), decoded)) {
            error_ = decoder_.error();
            fprintf(stderr, "[%s] MJPEG YUV decode failed: %s\n",
                    options_.camera_name, error_.c_str());
            return VideoFrameProcessResult::decode_failure;
        }
        timings_.decode_us += elapsed_since(decode_start);

        if ((options_.expected_width > 0 &&
             decoded.width != options_.expected_width) ||
            (options_.expected_height > 0 &&
             decoded.height != options_.expected_height)) {
            error_ = "decoded MJPEG dimensions changed";
            fprintf(stderr, "[%s] %s: %dx%d expected %dx%d\n",
                    options_.camera_name, error_.c_str(),
                    decoded.width, decoded.height,
                    options_.expected_width, options_.expected_height);
            return VideoFrameProcessResult::decode_failure;
        }

        bool imu_overflow = false;
        if (options_.has_imu && imu_queue_) {
            const auto imu_start = Clock::now();
            ImuFrame imu_frame;
            imu_frame.frame_idx = frame.frame_idx;
            imu_frame.pts_us = frame.pts_us;
            if (options_.imu_orientation == ImuOrientation::HORIZONTAL_TOP) {
                imu_frame.size = imu_read_luma_horizontal(
                    decoded.y.data, decoded.width, decoded.height,
                    decoded.y.stride, imu_frame.data.data());
            } else {
                imu_frame.size = imu_read_luma_vertical(
                    decoded.y.data, decoded.width, decoded.height,
                    decoded.y.stride, imu_frame.data.data());
            }
            if (imu_frame.size >= IMU_GROUP &&
                !imu_queue_->try_push(std::move(imu_frame))) {
                imu_overflow = true;
                fprintf(stderr, "[%s] IMU queue overflow\n",
                        options_.camera_name);
            }
            timings_.imu_us += elapsed_since(imu_start);
        }

        if (options_.output_h265) {
            if (!encoder_ || !h265_fp_) {
                error_ = "H.265 output is not initialized";
                return VideoFrameProcessResult::encoder_failure;
            }
            const auto nv12_start = Clock::now();
            if (!pack_yuv_to_nv12(
                    decoded, options_.nv12_stride, nv12_)) {
                error_ = "YUV to NV12 packing failed";
                fprintf(stderr, "[%s] %s\n",
                        options_.camera_name, error_.c_str());
                return VideoFrameProcessResult::decode_failure;
            }
            timings_.nv12_us += elapsed_since(nv12_start);

            const auto encoder_start = Clock::now();
            const MppPutResult result =
                encoder_->put(nv12_.data(), h265_fp_);
            timings_.encoder_us += elapsed_since(encoder_start);
            if (!result.ok) {
                error_ = "MPP encode or FIFO write failed";
                return VideoFrameProcessResult::encoder_failure;
            }
            total_h265_bytes_ += result.bytes;
        }

        export_preview_if_requested(frame);
        ++timings_.frames;
        if (imu_overflow) {
            return VideoFrameProcessResult::imu_queue_overflow;
        }
        return VideoFrameProcessResult::ok;
    }

    void finish() {
        if (imu_queue_) {
            imu_queue_->close();
        }
    }

    const char* error() const {
        return error_.c_str();
    }

    const VideoFrameProcessorTimings& timings() const {
        return timings_;
    }

    size_t total_h265_bytes() const {
        return total_h265_bytes_;
    }

private:
    using Clock = std::chrono::steady_clock;

    static uint64_t elapsed_since(Clock::time_point start) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - start)
                .count());
    }

    void export_preview_if_requested(const CompressedFrame& frame) {
        if (!control_) {
            return;
        }
        std::string preview_path;
        if (!control_->take_preview(preview_path)) {
            return;
        }
        const std::string temporary = preview_path + ".tmp";
        FILE* fp = fopen(temporary.c_str(), "wb");
        if (!fp) {
            return;
        }
        const size_t written =
            fwrite(frame.data.data(), 1, frame.data.size(), fp);
        const bool ok = written == frame.data.size() && fclose(fp) == 0;
        if (ok) {
            rename(temporary.c_str(), preview_path.c_str());
        } else {
            remove(temporary.c_str());
        }
    }

    VideoFrameProcessorOptions options_;
    MppEncoder* encoder_ = nullptr;
    FILE* h265_fp_ = nullptr;
    ImuFrameQueue* imu_queue_ = nullptr;
    VideoCaptureControl* control_ = nullptr;
    MjpegYuvDecoder decoder_;
    std::vector<uint8_t> nv12_;
    VideoFrameProcessorTimings timings_;
    size_t total_h265_bytes_ = 0;
    std::string error_;
};
