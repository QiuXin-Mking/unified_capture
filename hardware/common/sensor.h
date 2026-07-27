#pragma once
/*
 * sensor.h — Sensor 基类声明
 *
 * 每个 Sensor 在新 std::thread 中运行三段式生命周期:
 *   setup() → collect() → teardown()
 * 所有 sensor 线程在 setup() 完成后通过 SimpleBarrier 同步同时进入 collect().
 */

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "time_utils.h"
#include "frame_queue.h"

class SimpleBarrier;

class Sensor {
public:
    Sensor(std::string name, std::atomic<bool>& running);
    virtual ~Sensor() = default;

    // 在新线程里运行完整生命周期: setup → gate.wait → collect → teardown
    void launch(SimpleBarrier& gate);

    // 等待线程结束
    void join();

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
