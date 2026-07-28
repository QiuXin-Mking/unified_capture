#include "hardware/video/device_discovery.h"

#include <cstdio>
#include <vector>

extern "C" {
#include "Nori_Xvision_API.h"
}

namespace {

constexpr uint16_t kJhh2Vid = 0x1bcf;
constexpr uint16_t kJhh2Pid = 0x2d50;
constexpr uint16_t kSixVid = 0x1bcf;
constexpr uint16_t kSixPid = 0x2d51;

struct DeviceEntry {
    uint32_t id;
    uint16_t vid;
    uint16_t pid;
    uint32_t bus;
    uint32_t dev;
};

CameraDiscoveryResult initial_result() {
    CameraDiscoveryResult result;
    result.jhh2 = {{
        {{"jhh2_left", kJhh2Vid, kJhh2Pid, 0, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true, -1}, true},
        {{"jhh2_right", kJhh2Vid, kJhh2Pid, 1, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true, -1}, true},
    }};
    return result;
}

}  // namespace

void scan_devices() {
    uint32_t count = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &count);
    if (ret != NORI_OK) {
        printf("Nori_Xvision_Init failed: 0x%x\n", ret);
        return;
    }
    printf("Found %u device(s):\n", count);
    for (uint32_t i = 0; i < count; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);
        printf("  [%u] %04x:%04x \"%s\" \"%s\" %s\n",
               i, info.idVendor, info.idProduct,
               info.iManufacturer, info.iProduct, info.device);
        VERSION_INFO ver;
        if (Nori_Xvision_GetVersion(i, &ver) == NORI_OK) {
            printf("       SDK:%s  Type:%s\n", ver.SDKVersion, ver.DeviceType);
        }
    }
    Nori_Xvision_UnInit();
}

CameraDiscoveryResult discover_cameras() {
    CameraDiscoveryResult result = initial_result();

    uint32_t total_devices = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &total_devices);
    if (ret != NORI_OK) {
        fprintf(stderr, "ERROR: Nori_Xvision_Init failed: 0x%x\n", ret);
        return result;
    }
    printf("Nori Xvision SDK: found %u device(s)\n", total_devices);
    if (total_devices == 0) {
        Nori_Xvision_UnInit();
        return result;
    }

    std::vector<DeviceEntry> all;
    for (uint32_t i = 0; i < total_devices; i++) {
        DEVICE_INFO info;
        Nori_Xvision_GetDeviceInfo(i, &info);
        all.push_back({i, info.idVendor, info.idProduct, info.busnum, info.devnum});
        printf("  Device[%u]: %04x:%04x bus=%u dev=%u\n",
               i, info.idVendor, info.idProduct, info.busnum, info.devnum);
    }

    uint32_t sixcam_bus = 0;

    for (auto& device : all) {
        if (device.vid == kSixVid && device.pid == kSixPid) {
            result.sixcam.jhh04_id = device.id;
            result.sixcam.enabled = true;
            sixcam_bus = device.bus;
            printf("  %-12s -> device[%u] bus=%u  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh04", device.id, device.bus);
            result.active_count++;
            break;
        }
    }

    if (result.sixcam.enabled && sixcam_bus > 0) {
        for (auto& device : all) {
            if (device.vid == kJhh2Vid && device.pid == kJhh2Pid &&
                device.bus == sixcam_bus) {
                result.sixcam.jhh02_id = device.id;
                printf("  %-12s -> device[%u] bus=%u  4000x1200@30 IMU=Y (SixCam)\n",
                       "jhh02", device.id, device.bus);
                result.active_count++;
                break;
            }
        }
        if (!result.sixcam.jhh02_id) {
            fprintf(stderr, "WARN: jhh02 not found on SixCam bus %u\n", sixcam_bus);
            result.sixcam.enabled = false;
            result.active_count--;
        }
    }

    int jhh2_index = 0;
    for (auto& device : all) {
        if (device.vid != kJhh2Vid || device.pid != kJhh2Pid) {
            continue;
        }
        if (device.bus == sixcam_bus) {
            continue;
        }
        if (jhh2_index >= static_cast<int>(result.jhh2.size())) {
            break;
        }
        auto& camera = result.jhh2[jhh2_index];
        camera.config.device_id = static_cast<int>(device.id);
        camera.enabled = true;
        printf("  %-12s -> device[%u] bus=%u  %dx%d@%d IMU=%c\n",
               camera.config.name, device.id, device.bus,
               camera.config.width, camera.config.height, camera.config.fps,
               camera.config.has_imu ? 'Y' : 'N');
        jhh2_index++;
    }
    for (int i = jhh2_index; i < static_cast<int>(result.jhh2.size()); i++) {
        result.jhh2[i].enabled = false;
        printf("  %-12s -> disabled (no free 2d50 on other buses)\n",
               result.jhh2[i].config.name);
    }

    for (const auto& camera : result.jhh2) {
        if (camera.enabled && camera.config.device_id >= 0) {
            result.active_count++;
        }
    }
    return result;
}
