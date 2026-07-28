#include "hardware/wrist/wrist_discovery.h"

#include <string>
#include <vector>

namespace {

bool supports_target_format(const WristDeviceInfo& device) {
    const WristVideoFormat& target = wrist_target_format();
    for (const WristVideoFormat& format : device.formats) {
        if (format.is_mjpeg == target.is_mjpeg &&
            format.width == target.width && format.height == target.height &&
            format.fps == target.fps) {
            return true;
        }
    }
    return false;
}

std::string target_format_name() {
    const WristVideoFormat& target = wrist_target_format();
    return std::to_string(target.width) + "x" +
           std::to_string(target.height) + "@" + std::to_string(target.fps);
}

void mark_unavailable(WristDiscoveryResult* result, std::size_t index,
                      std::string error) {
    WristCameraSlot& slot = result->cameras[index];
    slot.error = std::move(error);
    result->errors.push_back(slot.error);
}

template <typename MakeConfig>
void match_slot(WristDiscoveryResult* result, std::size_t index,
                const std::string& logical_name, const std::string& product,
                const std::vector<WristDeviceInfo>& inventory,
                MakeConfig make_config) {
    std::vector<const WristDeviceInfo*> candidates;
    for (const WristDeviceInfo& device : inventory) {
        if (device.product == product) {
            candidates.push_back(&device);
        }
    }

    if (candidates.empty()) {
        mark_unavailable(result, index,
                         logical_name + ": missing product " + product);
        return;
    }
    if (candidates.size() != 1) {
        mark_unavailable(result, index,
                         logical_name + ": duplicate product " + product);
        return;
    }

    const WristDeviceInfo& device = *candidates.front();
    if (!supports_target_format(device)) {
        mark_unavailable(result, index,
                         logical_name + ": missing MJPEG " +
                             target_format_name());
        return;
    }

    WristCameraSlot& slot = result->cameras[index];
    slot.config = make_config(device.vid, device.pid, device.device_id);
    slot.available = true;
    ++result->active_count;
}

}  // namespace

WristDiscoveryResult match_wrist_cameras(
    const WristDeviceMap& device_map,
    const std::vector<WristDeviceInfo>& inventory) {
    WristDiscoveryResult result;
    match_slot(&result, 0, "wrist_left", device_map.left_product, inventory,
               make_wrist_left_config);
    match_slot(&result, 1, "wrist_right", device_map.right_product, inventory,
               make_wrist_right_config);
    result.degraded = device_map.allow_missing_devices && result.active_count != 2;
    return result;
}
