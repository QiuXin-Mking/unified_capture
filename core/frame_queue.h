#pragma once
/*
 * frame_queue.h — 线程安全 BGR 帧队列 (Video → IMU)
 *
 * Video 线程: try_push (非阻塞, 满了就丢)
 * IMU 线程:   pop_wait (阻塞等帧)
 * 停止时:     IMU 线程用 running || !empty() 清空残余帧
 */

#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

struct BGRFrame {
    uint64_t    frame_idx;
    uint64_t    pts_us;      // 距离 t0 的时间戳 (微秒)
    int         width;
    int         height;
    std::vector<uint8_t> data;  // BGR24 packed

    BGRFrame() = default;
    BGRFrame(uint64_t idx, uint64_t pts, int w, int h)
        : frame_idx(idx), pts_us(pts), width(w), height(h), data(w * h * 3) {}
};

class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 4) : max_size_(max_size) {}

    // 非阻塞 push, 满了返回 false (不等待, 不允许拖慢采集)
    bool try_push(BGRFrame&& frame) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (q_.size() >= max_size_) return false;
        q_.push(std::move(frame));
        cv_.notify_one();
        return true;
    }

    // 阻塞 pop, 队列空时等待
    BGRFrame pop_wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !q_.empty(); });
        BGRFrame f = std::move(q_.front());
        q_.pop();
        return f;
    }

    // 非阻塞 pop, 空时返回 false
    bool try_pop(BGRFrame& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return q_.empty();
    }

private:
    std::queue<BGRFrame> q_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    size_t max_size_;
};
