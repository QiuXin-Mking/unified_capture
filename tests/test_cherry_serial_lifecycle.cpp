#include "hardware/cherry/cherry_serial_sensor.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

int main()
{
    std::atomic<bool> running{true};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maximum_concurrent{0};
    std::promise<void> first_frame;
    auto first_frame_seen = first_frame.get_future();

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

    // setup() starts the reader immediately after ACK. The first post-ACK
    // frame must be consumed before collect()/barrier-phase waiting begins.
    assert(reader.start());
    assert(!reader.start());
    assert(first_frame_seen.wait_for(std::chrono::milliseconds(200)) ==
           std::future_status::ready);
    assert(calls.load() > 0);

    running = false;
    reader.wait_until_stopped();
    reader.stop_and_join();
    assert(maximum_concurrent.load() == 1);
    return 0;
}
