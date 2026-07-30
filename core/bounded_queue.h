#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

    bool try_push(T&& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push(std::move(value));
        ready_.notify_one();
        return true;
    }

    bool wait_push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        space_ready_.wait(lock, [this] {
            return closed_ || queue_.size() < capacity_;
        });
        if (closed_) {
            return false;
        }
        queue_.push(std::move(value));
        ready_.notify_one();
        return true;
    }

    bool wait_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        space_ready_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
        space_ready_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_ready_;
    std::queue<T> queue_;
    bool closed_ = false;
};
