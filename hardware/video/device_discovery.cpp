#include "hardware/video/device_discovery.h"
#include "hardware/wrist/wrist_discovery.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Nori_Xvision_API.h"

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
    result.sixcam.enabled = true;
    result.jhh2 = {{
        {{"jhh2_left", kJhh2Vid, kJhh2Pid, 0, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true, -1}, true},
        {{"jhh2_right", kJhh2Vid, kJhh2Pid, 1, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true, -1}, true},
    }};
    return result;
}

CameraDiscoveryResult discover_mango_cameras() {
    CameraDiscoveryResult result = initial_result();

    uint32_t total_devices = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &total_devices);
    if (ret != NORI_OK) {
        fprintf(stderr, "ERROR: Nori_Xvision_Init failed: 0x%x\n", ret);
        return result;
    }
    printf("Nori Xvision SDK: found %u device(s)\n", total_devices);
    if (total_devices == 0) {
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

std::string device_product_name(const DEVICE_INFO& info) {
    return std::string(reinterpret_cast<const char*>(info.iProduct));
}

CameraDiscoveryResult discover_banana_cameras(
    const ProductConfiguration& configuration) {
    CameraDiscoveryResult result;
    result.profile = ProductProfile::banana;

    uint32_t total_devices = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &total_devices);
    if (ret != NORI_OK) {
        char error[96];
        snprintf(error, sizeof(error), "Nori_Xvision_Init failed: 0x%x", ret);
        result.camera_errors.emplace_back(error);
        result.degraded = configuration.wrist.allow_missing_devices;
        fprintf(stderr, "ERROR: %s\n", error);
        return result;
    }

    printf("Nori Xvision SDK: found %u device(s)\n", total_devices);
    if (total_devices == 0) {
        result.degraded = configuration.wrist.allow_missing_devices;
        return result;
    }
    std::vector<WristDeviceInfo> inventory;
    inventory.reserve(total_devices);
    for (uint32_t i = 0; i < total_devices; ++i) {
        DEVICE_INFO info{};
        if (Nori_Xvision_GetDeviceInfo(i, &info) != NORI_OK) {
            fprintf(stderr, "WARN: unable to read Device[%u] information\n", i);
            continue;
        }

        WristDeviceInfo device;
        device.device_id = i;
        device.vid = info.idVendor;
        device.pid = info.idProduct;
        device.product = device_product_name(info);

        uint32_t format_count = 0;
        uint32_t format_ret = Nori_Xvision_GetDeviceVideoInfoSize(
            device.device_id, &format_count);
        if (format_ret != NORI_OK) {
            fprintf(stderr, "WARN: Device[%u] format count failed: 0x%x\n",
                    device.device_id, format_ret);
        } else {
            device.formats.reserve(format_count);
            for (uint32_t format_index = 0; format_index < format_count;
                 ++format_index) {
                VIDEO_INFO format{};
                format_ret = Nori_Xvision_GetDeviceVideoInfo(
                    device.device_id, format_index, &format);
                if (format_ret != NORI_OK) {
                    fprintf(stderr,
                            "WARN: Device[%u] format[%u] failed: 0x%x\n",
                            device.device_id, format_index, format_ret);
                    continue;
                }
                device.formats.push_back(
                    {format.u_Format == VIDEO_MEDIA_TYPE_MJPG,
                     static_cast<int>(format.u_Width),
                     static_cast<int>(format.u_Height),
                     static_cast<int>(format.f_Fps)});
            }
        }

        printf("  Device[%u]: %04x:%04x product=\"%s\" formats=%zu\n",
               device.device_id, device.vid, device.pid,
               device.product.c_str(), device.formats.size());
        inventory.push_back(std::move(device));
    }

    WristDiscoveryResult wrist =
        match_wrist_cameras(configuration.wrist, inventory);
    for (std::size_t i = 0; i < result.wrist.size(); ++i) {
        result.wrist[i].config = wrist.cameras[i].config;
        result.wrist[i].enabled = wrist.cameras[i].available;
    }
    result.degraded = wrist.degraded;
    result.camera_errors = std::move(wrist.errors);
    result.active_count = wrist.active_count;

    // ── 六目模块发现 (banana sixcam) ──
    if (configuration.sixcam_enabled) {
        uint32_t sixcam_bus = 0;

        // 找 JHH04 (1bcf:2d51)
        for (uint32_t i = 0; i < total_devices; i++) {
            DEVICE_INFO info{};
            if (Nori_Xvision_GetDeviceInfo(i, &info) != NORI_OK) continue;
            if (info.idVendor == kSixVid && info.idProduct == kSixPid) {
                result.sixcam.jhh04_id = static_cast<int>(i);
                result.sixcam.enabled = true;
                sixcam_bus = info.busnum;
                printf("  %-12s -> device[%u] bus=%u  %s (SixCam)\n",
                       "jhh04", i, info.busnum,
                       device_product_name(info).c_str());
                result.active_count++;
                break;
            }
        }

        // 找 JHH02 (1bcf:2d50, 同一 bus)
        if (result.sixcam.enabled && sixcam_bus > 0) {
            for (uint32_t i = 0; i < total_devices; i++) {
                DEVICE_INFO info{};
                if (Nori_Xvision_GetDeviceInfo(i, &info) != NORI_OK) continue;
                if (info.idVendor == kJhh2Vid && info.idProduct == kJhh2Pid &&
                    info.busnum == sixcam_bus) {
                    result.sixcam.jhh02_id = static_cast<int>(i);
                    printf("  %-12s -> device[%u] bus=%u  %s (SixCam)\n",
                           "jhh02", i, info.busnum,
                           device_product_name(info).c_str());
                    result.active_count++;
                    break;
                }
            }
            if (!result.sixcam.jhh02_id) {
                fprintf(stderr, "WARN: jhh02 not found on SixCam bus %u\n",
                        sixcam_bus);
                result.sixcam.enabled = false;
                result.active_count--;
                result.camera_errors.emplace_back(
                    "jhh02 not found on sixcam bus");
            }
        } else {
            result.sixcam.enabled = false;
            if (result.camera_errors.empty() || configuration.wrist.allow_missing_devices) {
                result.camera_errors.emplace_back("jhh04 sixcam not found");
            }
        }
    }

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

CameraDiscoveryResult discover_cameras(const ProductConfiguration& configuration) {
    if (configuration.profile == ProductProfile::banana) {
        return discover_banana_cameras(configuration);
    }
    return discover_mango_cameras();
}
