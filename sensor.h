#pragma once
/*
 * sensor.h — Sensor 基类 + 线程安全帧队列
 */

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <queue>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// ============================================================
// 统一时间工具 (所有 Sensor 共用)
// ============================================================
extern struct timespec g_t0;

static inline uint64_t elapsed_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - g_t0.tv_sec) * 1000000ULL +
           (uint64_t)(now.tv_nsec - g_t0.tv_nsec) / 1000ULL;
}

// ============================================================
// 递归创建目录 (等效 mkdir -p)
// ============================================================
static inline int mkdir_p(const char* path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

// ============================================================
// SimpleBarrier — C++20 std::barrier 替代 (兼容 GCC 10)
// ============================================================
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

// ============================================================
// Sensor 基类
// ============================================================
class Sensor {
public:
    Sensor(std::string name, std::atomic<bool>& running)
        : name_(std::move(name)), running_(running) {}
    virtual ~Sensor() = default;

    // 在一个新线程里运行完整的生命周期: setup → gate.wait → collect → teardown
    void launch(SimpleBarrier& gate) {
        thread_ = std::thread([this, &gate]() {
            setup();
            gate.arrive_and_wait();  // ★ 同步点: 所有线程就绪, 一起出发
            collect();
            teardown();
        });
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    const std::string& name() const { return name_; }

protected:
    std::string name_;
    std::atomic<bool>& running_;

    virtual void setup()   = 0;
    virtual void collect() = 0;
    virtual void teardown() = 0;

private:
    std::thread thread_;
};

// ============================================================
// FrameQueue — 线程安全 BGR 帧队列 (Video → IMU)
//
// Video 线程: try_push (非阻塞, 满了就丢)
// IMU 线程:   pop_wait (阻塞等帧)
// 停止时:     IMU 线程用 running || !empty() 清空残余帧
// ============================================================
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
