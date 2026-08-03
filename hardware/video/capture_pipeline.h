#pragma once

#include "hardware/video/compressed_frame_queue.h"
#include "hardware/video/v4l2_frame_view.h"
#include "hardware/video/video_pipeline_stats.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

enum class VideoFrameProcessResult {
    ok,
    decode_failure,
    encoder_failure,
    imu_queue_overflow,
};

template <typename CaptureSource>
bool capture_source_fatal(const CaptureSource& source) {
    if constexpr (requires { source.fatal_error(); }) {
        return source.fatal_error();
    }
    return false;
}

inline uint64_t capture_pipeline_now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

template <typename CaptureSource, typename Processor>
VideoPipelineStats run_capture_pipeline(CaptureSource& source,
                                        std::atomic<bool>& running,
                                        Processor& processor,
                                        size_t queue_capacity = 12) {
    CompressedFrameQueue queue(queue_capacity);
    VideoPipelineStats stats;
    V4l2SequenceTracker sequence_tracker;

    std::thread processing_thread([&] {
        CompressedFrame frame;
        while (queue.wait_pop(frame)) {
            const VideoFrameProcessResult result = processor.process(frame);
            ++stats.processed;
            if (result == VideoFrameProcessResult::decode_failure) {
                ++stats.decode_failures;
            } else if (result == VideoFrameProcessResult::encoder_failure) {
                ++stats.encoder_failures;
            } else if (result ==
                       VideoFrameProcessResult::imu_queue_overflow) {
                ++stats.imu_queue_overflows;
            }
        }
        processor.finish();
    });

    uint64_t frame_idx = 0;
    while (running) {
        if (!source.wait_for_frame(50)) {
            if (capture_source_fatal(source)) {
                running = false;
                break;
            }
            continue;
        }

        V4l2FrameView view;
        if (!source.dequeue_frame(view)) {
            if (capture_source_fatal(source)) {
                running = false;
                break;
            }
            continue;
        }

        sequence_tracker.observe(view.sequence, stats);
        CompressedFrame frame = copy_compressed_frame(
            view, frame_idx++, capture_pipeline_now_us());
        ++stats.acquired;

        if (!source.requeue_frame()) {
            running = false;
            break;
        }
        if (!queue.try_push(std::move(frame))) {
            ++stats.queue_overflows;
        }
    }

    queue.close();
    processing_thread.join();
    return stats;
}
