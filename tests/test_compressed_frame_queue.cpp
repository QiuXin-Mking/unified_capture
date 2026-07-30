#include "hardware/video/compressed_frame_queue.h"

#include <cassert>
#include <cstdint>
#include <thread>

int main() {
    CompressedFrameQueue queue(2);
    assert(queue.try_push(CompressedFrame{1, 10, 7, {1, 2}}));
    assert(queue.try_push(CompressedFrame{2, 20, 8, {3, 4}}));
    assert(!queue.try_push(CompressedFrame{3, 30, 9, {5, 6}}));

    queue.close();
    assert(!queue.try_push(CompressedFrame{4, 40, 10, {7, 8}}));

    CompressedFrame out;
    assert(queue.wait_pop(out));
    assert(out.frame_idx == 1);
    assert(out.pts_us == 10);
    assert(out.v4l2_sequence == 7);
    assert((out.data == std::vector<uint8_t>{1, 2}));

    assert(queue.wait_pop(out));
    assert(out.frame_idx == 2);
    assert((out.data == std::vector<uint8_t>{3, 4}));
    assert(!queue.wait_pop(out));
    assert(queue.empty());

    CompressedFrameQueue blocking_queue(1);
    bool popped = false;
    std::thread consumer([&] {
        CompressedFrame frame;
        popped = blocking_queue.wait_pop(frame);
        assert(frame.frame_idx == 9);
    });
    assert(blocking_queue.try_push(CompressedFrame{9, 90, 19, {9}}));
    blocking_queue.close();
    consumer.join();
    assert(popped);
}
