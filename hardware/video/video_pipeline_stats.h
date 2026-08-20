#pragma once

#include <cstdint>

struct VideoPipelineStats {
    uint64_t acquired = 0;
    uint64_t processed = 0;
    uint64_t decode_failures = 0;
    uint64_t queue_overflows = 0;
    uint64_t sequence_gaps = 0;
    uint64_t encoder_failures = 0;
    uint64_t imu_queue_overflows = 0;
    uint64_t y8_publish_failures = 0;
};

class V4l2SequenceTracker {
public:
    void observe(uint32_t sequence, VideoPipelineStats& stats) {
        if (initialized_) {
            const uint32_t expected = previous_ + 1;
            if (sequence != expected) {
                stats.sequence_gaps += static_cast<uint32_t>(sequence - expected);
            }
        }
        previous_ = sequence;
        initialized_ = true;
    }

private:
    uint32_t previous_ = 0;
    bool initialized_ = false;
};

inline bool video_pipeline_valid(const VideoPipelineStats& stats) {
    return stats.acquired == stats.processed &&
           stats.decode_failures == 0 &&
           stats.queue_overflows == 0 &&
           stats.sequence_gaps == 0 &&
           stats.encoder_failures == 0 &&
           stats.imu_queue_overflows == 0 &&
           stats.y8_publish_failures == 0;
}
