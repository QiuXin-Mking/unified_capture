#include "hardware/cherry/cherry_serial_sensor.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

int main()
{
    std::atomic<bool> running{true};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maximum_concurrent{0};
    std::promise<void> first_frame;
    auto first_frame_seen = first_frame.get_future();
    std::vector<std::string> events;

    cherry::SerialReadLoop reader(running, [&](int) {
        const int active = concurrent.fetch_add(1) + 1;
        int maximum = maximum_concurrent.load();
        while (active > maximum &&
               !maximum_concurrent.compare_exchange_weak(maximum, active)) {
        }
        if (calls.fetch_add(1) == 0) first_frame.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        concurrent.fetch_sub(1);
        return true;
    });

    cherry::SerialLifecycleCoordinator lifecycle;
    events.push_back("ack");
    assert(lifecycle.after_ack(
        true,
        [&] {
            events.push_back("reader.start");
            return reader.start();
        },
        [&] { events.push_back("mark_ready"); }));
    assert((events == std::vector<std::string>{
                          "ack", "reader.start", "mark_ready"}));
    assert(!reader.start());
    assert(first_frame_seen.wait_for(std::chrono::milliseconds(200)) ==
           std::future_status::ready);
    assert(calls.load() > 0);

    running = false;
    lifecycle.before_stop(
        [&] {
            reader.wait_until_stopped();
            reader.stop_and_join();
            events.push_back("reader.join");
        },
        [&] { events.push_back("STOP"); });
    assert(events[events.size() - 2] == "reader.join");
    assert(events.back() == "STOP");
    assert(maximum_concurrent.load() == 1);
    return 0;
}
