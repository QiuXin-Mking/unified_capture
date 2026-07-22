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

#include "time_utils.h"
#include "barrier.h"
#include "frame_queue.h"

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
