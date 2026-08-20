#include "hardware/video/video_pipeline_stats.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    VideoPipelineStats stats;
    stats.acquired = 30;
    stats.processed = 30;
    assert(video_pipeline_valid(stats));

    stats.processed = 29;
    assert(!video_pipeline_valid(stats));
    stats.processed = 30;

    stats.decode_failures = 1;
    assert(!video_pipeline_valid(stats));
    stats.decode_failures = 0;
    stats.queue_overflows = 1;
    assert(!video_pipeline_valid(stats));
    stats.queue_overflows = 0;
    stats.sequence_gaps = 1;
    assert(!video_pipeline_valid(stats));
    stats.sequence_gaps = 0;
    stats.encoder_failures = 1;
    assert(!video_pipeline_valid(stats));
    stats.encoder_failures = 0;
    stats.imu_queue_overflows = 1;
    assert(!video_pipeline_valid(stats));
    stats.imu_queue_overflows = 0;
    stats.y8_publish_failures = 1;
    assert(!video_pipeline_valid(stats));
    stats.y8_publish_failures = 0;
    assert(video_pipeline_valid(stats));

    V4l2SequenceTracker tracker;
    tracker.observe(10, stats);
    tracker.observe(11, stats);
    assert(stats.sequence_gaps == 0);
    tracker.observe(14, stats);
    assert(stats.sequence_gaps == 2);

    VideoPipelineStats wrap_stats;
    V4l2SequenceTracker wrap_tracker;
    wrap_tracker.observe(std::numeric_limits<uint32_t>::max(), wrap_stats);
    wrap_tracker.observe(0, wrap_stats);
    assert(wrap_stats.sequence_gaps == 0);
}
