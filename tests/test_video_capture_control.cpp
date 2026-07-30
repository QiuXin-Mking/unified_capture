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
    control.request_preview("/tmp/preview.jpg");
    std::string path;
    assert(control.take_preview(path));
    assert(path == "/tmp/preview.jpg");
    assert(!control.take_preview(path));
}
