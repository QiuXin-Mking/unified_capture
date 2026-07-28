#pragma once
/*
 * barrier.h — C++20 std::barrier 替代 (兼容 GCC 10)
 */

#include <chrono>
#include <mutex>
#include <condition_variable>

class SimpleBarrier {
public:
    explicit SimpleBarrier(size_t count) : count_(count) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        size_t gen = generation_;
        if (++arrived_ == count_) {
            arrived_ = 0;
            generation_++;
            cv_.notify_all();
        } else {
            cv_.wait(lock, [this, gen] { return gen != generation_; });
        }
    }

    // main 线程用: 不入 barrier, 仅等待所有 sensor 线程到达
    // 返回 true=所有到达, false=超时
    bool wait_all_arrived(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (arrived_ >= count_) return true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t gen = generation_;
        return cv_.wait_until(lock, deadline, [this, gen] { return gen != generation_; });
    }

private:
    size_t count_;
    size_t arrived_ = 0;
    size_t generation_ = 0;
    std::mutex mtx_;
    std::condition_variable cv_;
};
