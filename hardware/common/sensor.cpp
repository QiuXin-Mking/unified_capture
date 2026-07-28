/*
 * sensor.cpp — Sensor 基类实现
 */

#include "hardware/common/sensor.h"
#include "core/barrier.h"

Sensor::Sensor(std::string name, std::atomic<bool>& running)
    : name_(std::move(name)), running_(running) {}

void Sensor::launch(SimpleBarrier& gate) {
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

void Sensor::join() {
    fprintf(stderr, "[%s] DBG: join() called, joining thread...\n", name_.c_str());
    if (thread_.joinable()) thread_.join();
    fprintf(stderr, "[%s] DBG: join() done\n", name_.c_str());
}
