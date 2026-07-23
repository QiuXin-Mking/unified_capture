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
            fprintf(stderr, "[%s] DBG: thread started, entering setup...\n", name_.c_str());
            setup();
            fprintf(stderr, "[%s] DBG: setup done, waiting at barrier...\n", name_.c_str());
            gate.arrive_and_wait();  // ★ 同步点: 所有线程就绪, 一起出发
            fprintf(stderr, "[%s] DBG: barrier released, entering collect...\n", name_.c_str());
            collect();
            fprintf(stderr, "[%s] DBG: collect done, entering teardown...\n", name_.c_str());
            teardown();
            fprintf(stderr, "[%s] DBG: teardown done, thread exiting\n", name_.c_str());
        });
    }

    void join() {
        fprintf(stderr, "[%s] DBG: join() called, joining thread...\n", name_.c_str());
        if (thread_.joinable()) thread_.join();
        fprintf(stderr, "[%s] DBG: join() done\n", name_.c_str());
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
