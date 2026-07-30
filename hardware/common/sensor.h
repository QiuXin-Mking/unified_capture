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

#include "core/time_utils.h"
// @deprecated: core/frame_queue.h 已废弃 (BGRFrame/FrameQueue 无生产引用)，
// Sensor 基类不依赖它，保留此 include 仅为兼容历史代码，后续可删除。
#include "core/frame_queue.h"

class SimpleBarrier;

class Sensor {
public:
    // 构造函数
    // name：传感器名称
    // running：原子的 布尔的 开关
    Sensor(std::string name, std::atomic<bool>& running);
    // 析构函数： 让编译器生成“默认析构函数体”
    virtual ~Sensor() = default;

    // 在新线程里运行完整生命周期: setup → gate.wait → collect → teardown
    void launch(SimpleBarrier& gate);

    // 等待线程结束
    void join();

    const std::string& name() const { return name_; }

protected:
    std::string name_;
    std::atomic<bool>& running_;

    // virtual 虚函数支持多状
    // = 0 纯虚函数：这个函数没有实现，必须在子类中重写
    // 语义就是 这事情我管过不了，子类全权负责
    virtual void setup()   = 0;
    virtual void collect() = 0;
    virtual void teardown() = 0;

private:
    std::thread thread_;
};
