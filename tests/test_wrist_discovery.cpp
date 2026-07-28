#include "hardware/wrist/wrist_discovery.h"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace {

WristDeviceInfo device(uint32_t device_id, const std::string& product,
                       std::vector<WristVideoFormat> formats) {
    return WristDeviceInfo{device_id, 0x1bcf, 0x2d50, product, std::move(formats)};
}

void assert_wrist_encoding(const WristDiscoveryResult& result) {
    for (const auto& camera : result.cameras) {
        assert(camera.config.output_h265);
        assert(!camera.config.output_y8);
        assert(camera.config.imu_orientation == ImuOrientation::VERTICAL_LEFT);
    }
}

}  // namespace

int main() {
    const WristDeviceMap map{true, "SL", "JHHSW"};
    const WristVideoFormat target{true, 1440, 960, 30};

    const std::vector<WristDeviceInfo> complete_inventory{
        device(4, "SL", {target}),
        device(9, "JHHSW", {target}),
    };
    const WristDiscoveryResult complete =
        match_wrist_cameras(map, complete_inventory);
    assert(complete.active_count == 2);
    assert(!complete.degraded);
    assert(complete.cameras[0].available);
    assert(complete.cameras[1].available);
    assert(std::string(complete.cameras[0].config.name) == "wrist_left");
    assert(std::string(complete.cameras[1].config.name) == "wrist_right");
    assert_wrist_encoding(complete);

    const WristDiscoveryResult missing_right = match_wrist_cameras(
        map, {device(4, "SL", {target})});
    assert(missing_right.active_count == 1);
    assert(missing_right.degraded);
    assert(!missing_right.cameras[1].available);
    assert(missing_right.cameras[1].error.find("missing") != std::string::npos);
    assert(missing_right.errors.size() == 1);

    const WristDiscoveryResult wrong_product = match_wrist_cameras(
        map, {device(4, "SL-extra", {target}), device(9, "JHHSW", {target})});
    assert(!wrong_product.cameras[0].available);
    assert(wrong_product.cameras[0].config.device_id == -1);
    assert(wrong_product.active_count == 1);

    const WristDiscoveryResult duplicate_left = match_wrist_cameras(
        map, {device(4, "SL", {target}), device(5, "SL", {target}),
              device(9, "JHHSW", {target})});
    assert(!duplicate_left.cameras[0].available);
    assert(duplicate_left.cameras[0].config.device_id == -1);
    assert(duplicate_left.cameras[0].error.find("duplicate") != std::string::npos);

    const WristDiscoveryResult wrong_format = match_wrist_cameras(
        map, {device(4, "SL", {{true, 1280, 720, 30}}),
              device(9, "JHHSW", {target})});
    assert(!wrong_format.cameras[0].available);
    assert(wrong_format.cameras[0].config.device_id == -1);
    assert(wrong_format.cameras[0].error.find("1440x960@30") !=
           std::string::npos);
    return 0;
}
