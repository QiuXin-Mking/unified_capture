#pragma once
/*
 * barrier.h — C++20 std::barrier 替代 (兼容 GCC 10)
 */

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

private:
    size_t count_;
    size_t arrived_ = 0;
    size_t generation_ = 0;
    std::mutex mtx_;
    std::condition_variable cv_;
};
