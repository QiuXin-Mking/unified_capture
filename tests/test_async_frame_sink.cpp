#include "hardware/video/async_frame_sink.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct FakeSinkResult {
    bool ok;
    size_t bytes;
};

int main() {
    std::mutex mutex;
    std::vector<uint8_t> received;
    auto sink = [&](const uint8_t* data, size_t size) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lock(mutex);
        received.insert(received.end(), data, data + size);
        return FakeSinkResult{true, size};
    };

    AsyncFrameSink<decltype(sink)> worker(2, sink);
    const std::vector<uint8_t> first{1, 2};
    const std::vector<uint8_t> second{3};
    const std::vector<uint8_t> third{4, 5, 6};
    assert(worker.submit(first.data(), first.size()));
    assert(worker.submit(second.data(), second.size()));
    assert(worker.submit(third.data(), third.size()));
    worker.finish();

    assert((received == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    assert(worker.frames() == 3);
    assert(worker.bytes() == 6);
    assert(worker.processing_us() > 0);
    assert(worker.ok());
    assert(!worker.submit(first.data(), first.size()));
}
