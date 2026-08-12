#include "hardware/video/capture_control.h"

#include <cassert>
#include <string>

int main() {
    VideoCaptureControl control;
    control.reset_stream_start(2, true);
    assert(control.jhh2_remaining.load() == 3);
    assert(!control.jhh02_init_done.load());
    assert(!control.wrists_may_start());
    assert(!control.jhh04_may_start());
    control.mark_jhh02_started();
    assert(control.wrists_may_start());
    assert(control.jhh2_remaining.load() == 2);
    assert(!control.jhh04_may_start());
    control.mark_wrist_started();
    assert(control.jhh2_remaining.load() == 1);
    assert(!control.jhh04_may_start());
    control.mark_wrist_started();
    assert(control.jhh2_remaining.load() == 0);
    assert(control.jhh04_may_start());

    control.reset_stream_start(2, false);
    assert(control.jhh2_remaining.load() == 2);
    assert(control.jhh02_init_done.load());
    std::string path;
    control.request_preview("jhh02", "/tmp/head.jpg");
    control.request_preview("wrist_left", "/tmp/left.jpg");
    assert(!control.take_preview("jhh04", path));
    assert(control.take_preview("wrist_left", path));
    assert(path == "/tmp/left.jpg");
    assert(control.take_preview("jhh02", path));
    assert(path == "/tmp/head.jpg");

    control.request_preview("jhh02", "/tmp/head-2.jpg");
    control.request_preview("wrist_right", "/tmp/right.jpg");
    assert(control.take_preview("wrist_right", path));
    assert(path == "/tmp/right.jpg");
    assert(control.take_preview("jhh02", path));
    assert(path == "/tmp/head-2.jpg");

    control.request_preview("", "/tmp/legacy.jpg");
    assert(control.take_preview("jhh04", path));
    assert(path == "/tmp/legacy.jpg");
    assert(!control.take_preview("jhh02", path));
}
