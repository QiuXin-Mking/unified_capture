#include "hardware/video/capture_pipeline.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace {

class FakeCaptureSource {
public:
    FakeCaptureSource(std::atomic<bool>& running, int frame_count)
        : running_(running) {
        for (int i = 0; i < frame_count; ++i) {
            frames_.push_back(
                {static_cast<uint8_t>(i), static_cast<uint8_t>(i + 1)});
        }
    }

    bool wait_for_frame(int) {
        if (next_ < frames_.size()) {
            return true;
        }
        running_ = false;
        return false;
    }

    bool dequeue_frame(V4l2FrameView& view) {
        if (next_ >= frames_.size()) {
            return false;
        }
        view = {frames_[next_].data(), frames_[next_].size(),
                static_cast<uint32_t>(100 + next_),
                static_cast<uint64_t>(1000 + next_)};
        return true;
    }

    bool requeue_frame() {
        ++requeued_;
        ++next_;
        if (next_ == frames_.size()) {
            running_ = false;
        }
        return true;
    }

    size_t requeued() const {
        return requeued_.load();
    }

private:
    std::atomic<bool>& running_;
    std::vector<std::vector<uint8_t>> frames_;
    size_t next_ = 0;
    std::atomic<size_t> requeued_{0};
};

class RecordingProcessor {
public:
    explicit RecordingProcessor(const FakeCaptureSource& source,
                                int delay_ms = 0)
        : source_(source), delay_ms_(delay_ms) {}

    VideoFrameProcessResult process(const CompressedFrame& frame) {
        if (source_.requeued() < frame.frame_idx + 1) {
            processed_before_requeue_ = true;
        }
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        order_.push_back(frame.frame_idx);
        return VideoFrameProcessResult::ok;
    }

    void finish() {
        finished_ = true;
    }

    const std::vector<uint64_t>& order() const {
        return order_;
    }

    bool processed_before_requeue() const {
        return processed_before_requeue_;
    }

    bool finished() const {
        return finished_;
    }

private:
    const FakeCaptureSource& source_;
    int delay_ms_ = 0;
    std::vector<uint64_t> order_;
    bool processed_before_requeue_ = false;
    bool finished_ = false;
};

class NoopProcessor {
public:
    VideoFrameProcessResult process(const CompressedFrame&) {
        return VideoFrameProcessResult::ok;
    }
    void finish() {}
};

class FatalWaitSource {
public:
    bool wait_for_frame(int) { return false; }
    bool dequeue_frame(V4l2FrameView&) { return false; }
    bool requeue_frame() { return true; }
    bool fatal_error() const { return true; }
};

class FatalDequeueSource {
public:
    bool wait_for_frame(int) { return true; }
    bool dequeue_frame(V4l2FrameView&) { return false; }
    bool requeue_frame() { return true; }
    bool fatal_error() const { return true; }
};

class RequeueFailureSource {
public:
    bool wait_for_frame(int) { return true; }
    bool dequeue_frame(V4l2FrameView& view) {
        view = {bytes_, sizeof(bytes_), 1, 2};
        return true;
    }
    bool requeue_frame() { return false; }

private:
    uint8_t bytes_[2] = {0x01, 0x02};
};

class NoFrameSource {
public:
    bool wait_for_frame(int) {
        ++waits_;
        return false;
    }
    bool dequeue_frame(V4l2FrameView&) { return false; }
    bool requeue_frame() { return true; }
    bool fatal_error() const { return false; }
    int waits() const { return waits_; }

private:
    int waits_ = 0;
};

template <typename Source>
void assert_fatal_source_stops_pipeline(Source& source) {
    std::atomic<bool> running{true};
    NoopProcessor processor;
    auto result = std::async(std::launch::async, [&] {
        return run_capture_pipeline(source, running, processor, 1);
    });
    const bool returned = result.wait_for(std::chrono::milliseconds(200)) ==
                          std::future_status::ready;
    if (!returned) running = false;
    result.get();
    assert(returned);
    assert(!running);
}

}  // namespace

int main() {
    {
        std::atomic<bool> running{true};
        NoFrameSource raw_source;
        int health_checks = 0;
        SupervisedCaptureSource source(raw_source, [&] {
            ++health_checks;
            return health_checks < 3;
        });
        NoopProcessor processor;
        const VideoPipelineStats stats =
            run_capture_pipeline(source, running, processor, 1);
        assert(!running);
        assert(source.fatal_error());
        assert(raw_source.waits() >= 1);
        assert(health_checks >= 3);
        assert(stats.acquired == 0);
    }

    {
        FatalWaitSource source;
        assert_fatal_source_stops_pipeline(source);
    }

    {
        FatalDequeueSource source;
        assert_fatal_source_stops_pipeline(source);
    }

    {
        RequeueFailureSource source;
        assert_fatal_source_stops_pipeline(source);
    }

    {
        std::atomic<bool> running{true};
        FakeCaptureSource source(running, 3);
        RecordingProcessor processor(source);
        VideoPipelineStats stats =
            run_capture_pipeline(source, running, processor, 12);

        assert(stats.acquired == 3);
        assert(stats.processed == 3);
        assert(stats.queue_overflows == 0);
        assert(stats.sequence_gaps == 0);
        assert((processor.order() == std::vector<uint64_t>{0, 1, 2}));
        assert(!processor.processed_before_requeue());
        assert(processor.finished());
        assert(video_pipeline_valid(stats));
    }

    {
        std::atomic<bool> running{true};
        FakeCaptureSource source(running, 100);
        RecordingProcessor processor(source, 2);
        VideoPipelineStats stats =
            run_capture_pipeline(source, running, processor, 1);

        assert(stats.acquired == 100);
        assert(stats.queue_overflows > 0);
        assert(stats.processed + stats.queue_overflows == stats.acquired);
        assert(!video_pipeline_valid(stats));
        assert(processor.finished());
    }
}
