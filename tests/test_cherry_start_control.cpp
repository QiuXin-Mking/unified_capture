#include "hardware/cherry/cherry_start_control.h"

#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void test_ready_wakes_waiter()
{
    CherryStartControl control;
    auto result = std::async(std::launch::async, [&] {
        return control.wait(2s);
    });

    std::this_thread::sleep_for(10ms);
    control.mark_ready();

    assert(result.wait_for(200ms) == std::future_status::ready);
    const CherryStartResult value = result.get();
    assert(value.ready);
    assert(!value.timed_out);
    assert(value.error.empty());
}

void test_failure_wakes_waiter_with_exact_error()
{
    CherryStartControl control;
    auto result = std::async(std::launch::async, [&] {
        return control.wait(2s);
    });

    std::this_thread::sleep_for(10ms);
    control.mark_failed("START response mask mismatch");

    assert(result.wait_for(200ms) == std::future_status::ready);
    const CherryStartResult value = result.get();
    assert(!value.ready);
    assert(!value.timed_out);
    assert(value.error == "START response mask mismatch");
}

void test_pending_wait_times_out_without_deadlock()
{
    CherryStartControl control;
    const auto begin = std::chrono::steady_clock::now();
    const CherryStartResult value = control.wait(30ms);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    assert(!value.ready);
    assert(value.timed_out);
    assert(value.error == "timed out waiting for Cherry serial START");
    assert(elapsed >= 20ms);
    assert(elapsed < 500ms);
}

} // namespace

int main()
{
    test_ready_wakes_waiter();
    test_failure_wakes_waiter_with_exact_error();
    test_pending_wait_times_out_without_deadlock();
    return 0;
}
