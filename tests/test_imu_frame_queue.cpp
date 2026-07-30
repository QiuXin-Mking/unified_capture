#include "hardware/imu/imu_frame_queue.h"

#include <cassert>
#include <type_traits>

int main() {
    static_assert(std::is_trivially_destructible_v<ImuFrame>);
    static_assert(sizeof(ImuFrame) <= 288);

    ImuFrame input;
    input.frame_idx = 42;
    input.pts_us = 123456;
    input.size = 3;
    input.data[0] = 7;
    input.data[1] = 8;
    input.data[2] = 9;

    ImuFrameQueue queue(1);
    assert(queue.try_push(std::move(input)));
    queue.close();

    ImuFrame output;
    assert(queue.wait_pop(output));
    assert(output.frame_idx == 42);
    assert(output.pts_us == 123456);
    assert(output.size == 3);
    assert(output.data[0] == 7);
    assert(output.data[1] == 8);
    assert(output.data[2] == 9);
    assert(!queue.wait_pop(output));
}
