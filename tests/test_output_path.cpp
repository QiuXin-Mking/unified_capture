#include "core/output_path.h"

#include <cassert>
#include <string>

int main() {
    const std::string generated = capture_output_prefix("");
    assert(generated.rfind("/media/usb0/capture/record_", 0) == 0);

    assert(capture_output_prefix("manual_check") ==
           "/media/usb0/capture/manual_check");
    assert(capture_output_prefix("/media/usb0/capture/session") ==
           "/media/usb0/capture/session");

    assert(is_sd_capture_path("/media/usb0/capture"));
    assert(is_sd_capture_path("/media/usb0/capture/session_001"));
    assert(!is_sd_capture_path("/media/usb0/capture_old/session_001"));
    assert(!is_sd_capture_path("/root/capture/session_001"));
    return 0;
}
