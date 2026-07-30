#pragma once

#include "core/bounded_queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

template <typename Sink>
class AsyncFrameSink {
public:
    AsyncFrameSink(size_t capacity, Sink sink)
        : queue_(capacity)
        , sink_(std::move(sink))
        , thread_([this] { run(); }) {}

    ~AsyncFrameSink() {
        finish();
    }

    AsyncFrameSink(const AsyncFrameSink&) = delete;
    AsyncFrameSink& operator=(const AsyncFrameSink&) = delete;

    bool submit(const uint8_t* data, size_t size) {
        if (!data || size == 0 || finished_ || failed_) {
            return false;
        }
        std::vector<uint8_t> frame(data, data + size);
        return queue_.wait_push(std::move(frame));
    }

    void finish() {
        if (finished_.exchange(true)) {
            return;
        }
        queue_.close();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool ok() const {
        return !failed_;
    }

    uint64_t frames() const {
        return frames_;
    }

    size_t bytes() const {
        return bytes_;
    }

    uint64_t processing_us() const {
        return processing_us_;
    }

private:
    using Clock = std::chrono::steady_clock;

    void run() {
        std::vector<uint8_t> frame;
        while (queue_.wait_pop(frame)) {
            const auto start = Clock::now();
            const auto result = sink_(frame.data(), frame.size());
            processing_us_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - start)
                    .count());
            if (!result.ok) {
                failed_ = true;
                continue;
            }
            ++frames_;
            bytes_ += result.bytes;
        }
    }

    BoundedQueue<std::vector<uint8_t>> queue_;
    Sink sink_;
    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<size_t> bytes_{0};
    std::atomic<uint64_t> processing_us_{0};
};
