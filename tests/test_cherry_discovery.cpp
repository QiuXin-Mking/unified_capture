#include "hardware/cherry/cherry_discovery.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kCherryVid = 0x5268;
constexpr uint16_t kCherryPid = 0x1218;

CherryVideoEndpoint video(const std::string& path,
                          const std::string& usb_parent,
                          uint32_t bus = 2,
                          bool supports_target = true,
                          uint16_t vid = kCherryVid,
                          uint16_t pid = kCherryPid) {
    return {path, usb_parent, vid, pid, bus, supports_target};
}

CherrySerialEndpoint serial(const std::string& path,
                            const std::string& usb_parent,
                            uint32_t bus = 2,
                            uint16_t vid = kCherryVid,
                            uint16_t pid = kCherryPid) {
    return {path, usb_parent, vid, pid, bus};
}

void assert_unavailable(const CherryDiscoveryResult& result) {
    assert(!result.available);
    assert(result.video_path.empty());
    assert(result.serial_path.empty());
    assert(!result.error.empty());
}

}  // namespace

int main() {
    const CherryDeviceMap map{};
    const std::string parent = "/sys/devices/platform/usb2/2-1/2-1.1";

    // Same-bus endpoints on other physical USB parents must not be paired.
    // A metadata node on the correct parent must not create ambiguity.
    const CherryDiscoveryResult paired = match_cherry_device(
        map,
        {video("/dev/video0", parent),
         video("/dev/video1", parent, 2, false),
         video("/dev/video4", "/sys/devices/platform/usb2/2-1/2-1.4")},
        {serial("/dev/ttyACM0", parent),
         serial("/dev/ttyACM4", "/sys/devices/platform/usb2/2-1/2-1.5")});
    assert(paired.available);
    assert(paired.video_path == "/dev/video0");
    assert(paired.serial_path == "/dev/ttyACM0");
    assert(paired.usb_parent == parent);
    assert(paired.bus == 2);
    assert(paired.error.empty());

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent)},
        {serial("/dev/ttyACM0", "/sys/devices/platform/usb2/2-1/2-1.2")}));

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent, 2, true, 0x1234, kCherryPid)},
        {serial("/dev/ttyACM0", parent)}));
    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent)},
        {serial("/dev/ttyACM0", parent, 2, kCherryVid, 0xabcd)}));

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent, 2, false)},
        {serial("/dev/ttyACM0", parent)}));

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent), video("/dev/video2", parent)},
        {serial("/dev/ttyACM0", parent)}));

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video0", parent)},
        {serial("/dev/ttyACM0", parent), serial("/dev/ttyACM1", parent)}));

    assert_unavailable(match_cherry_device(
        map,
        {video("/dev/video1", parent, 2, false)},
        {serial("/dev/ttyACM0", parent)}));
    return 0;
}
