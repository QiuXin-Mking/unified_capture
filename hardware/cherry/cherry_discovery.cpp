#include "hardware/cherry/cherry_discovery.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

namespace {

struct ParentCandidates {
    std::vector<const CherryVideoEndpoint*> videos;
    std::vector<const CherrySerialEndpoint*> serials;
};

#ifdef __linux__

constexpr uint32_t kTargetPixelFormat = V4L2_PIX_FMT_H264;
constexpr uint32_t kTargetWidth = 3200;
constexpr uint32_t kTargetHeight = 1200;
constexpr uint32_t kTargetIntervalNumerator = 1;
constexpr uint32_t kTargetIntervalDenominator = 30;

struct UsbDeviceInfo {
    std::string parent;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint32_t bus = 0;
};

std::string read_sysfs_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return {};

    char buffer[256] = {};
    std::string result;
    if (fgets(buffer, sizeof(buffer), file)) {
        std::size_t length = strlen(buffer);
        while (length > 0 &&
               (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
            buffer[--length] = '\0';
        }
        result = buffer;
    }
    fclose(file);
    return result;
}

uint16_t read_sysfs_hex(const std::string& path) {
    const std::string text = read_sysfs_file(path);
    if (text.empty()) return 0;
    return static_cast<uint16_t>(strtoul(text.c_str(), nullptr, 16));
}

uint32_t read_sysfs_uint(const std::string& path) {
    const std::string text = read_sysfs_file(path);
    if (text.empty()) return 0;
    return static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 10));
}

std::string parent_path(const std::string& path) {
    const std::size_t separator = path.rfind('/');
    if (separator == std::string::npos) return {};
    if (separator == 0) return "/";
    return path.substr(0, separator);
}

bool find_usb_device(const std::string& class_path, UsbDeviceInfo* output) {
    char canonical[PATH_MAX] = {};
    if (!realpath(class_path.c_str(), canonical)) return false;

    std::string current = canonical;
    while (!current.empty()) {
        const std::string vendor_path = current + "/idVendor";
        const std::string product_path = current + "/idProduct";
        if (access(vendor_path.c_str(), R_OK) == 0 &&
            access(product_path.c_str(), R_OK) == 0) {
            output->parent = current;
            output->vid = read_sysfs_hex(vendor_path);
            output->pid = read_sysfs_hex(product_path);
            output->bus = read_sysfs_uint(current + "/busnum");
            return output->vid != 0;
        }

        const std::string parent = parent_path(current);
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return false;
}

bool supports_cherry_target(const std::string& device_path) {
    const int fd = open(device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct v4l2_capability capability = {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        close(fd);
        return false;
    }
    const uint32_t capabilities =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps
            : capability.capabilities;
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        return false;
    }

    bool supported = false;
    struct v4l2_fmtdesc format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (format.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
         ++format.index) {
        if (format.pixelformat != kTargetPixelFormat) continue;

        struct v4l2_frmsizeenum size = {};
        size.pixel_format = kTargetPixelFormat;
        for (size.index = 0;
             ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE ||
                size.discrete.width != kTargetWidth ||
                size.discrete.height != kTargetHeight) {
                continue;
            }

            struct v4l2_frmivalenum interval = {};
            interval.pixel_format = kTargetPixelFormat;
            interval.width = kTargetWidth;
            interval.height = kTargetHeight;
            for (interval.index = 0;
                 ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0;
                 ++interval.index) {
                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
                    interval.discrete.numerator ==
                        kTargetIntervalNumerator &&
                    interval.discrete.denominator ==
                        kTargetIntervalDenominator) {
                    supported = true;
                    break;
                }
            }
            if (supported) break;
        }
        if (supported) break;
    }

    close(fd);
    return supported;
}

#endif

CherryDiscoveryResult unavailable(std::string error) {
    CherryDiscoveryResult result;
    result.error = std::move(error);
    return result;
}

}  // namespace

CherryDiscoveryResult match_cherry_device(
    const CherryDeviceMap& device_map,
    const std::vector<CherryVideoEndpoint>& videos,
    const std::vector<CherrySerialEndpoint>& serials) {
    std::map<std::string, ParentCandidates> by_parent;
    for (const CherryVideoEndpoint& video : videos) {
        if (video.vid == device_map.vid && video.pid == device_map.pid &&
            video.supports_target && !video.usb_parent.empty()) {
            by_parent[video.usb_parent].videos.push_back(&video);
        }
    }
    for (const CherrySerialEndpoint& serial : serials) {
        if (serial.vid == device_map.vid && serial.pid == device_map.pid &&
            !serial.usb_parent.empty()) {
            by_parent[serial.usb_parent].serials.push_back(&serial);
        }
    }

    std::vector<const ParentCandidates*> complete;
    std::vector<std::string> complete_parents;
    for (const auto& [parent, candidates] : by_parent) {
        if (!candidates.videos.empty() && !candidates.serials.empty()) {
            complete.push_back(&candidates);
            complete_parents.push_back(parent);
        }
    }

    if (complete.empty()) {
        return unavailable(
            "cherry_stereo: no H264 3200x1200@30 video and ttyACM pair "
            "on the same USB parent");
    }
    if (complete.size() != 1) {
        return unavailable(
            "cherry_stereo: multiple complete USB-parent device pairs");
    }

    const ParentCandidates& candidates = *complete.front();
    if (candidates.videos.size() != 1) {
        return unavailable(
            "cherry_stereo: duplicate H264 capture endpoints on USB parent " +
            complete_parents.front());
    }
    if (candidates.serials.size() != 1) {
        return unavailable(
            "cherry_stereo: duplicate ttyACM endpoints on USB parent " +
            complete_parents.front());
    }

    const CherryVideoEndpoint& video = *candidates.videos.front();
    const CherrySerialEndpoint& serial = *candidates.serials.front();
    CherryDiscoveryResult result;
    result.available = true;
    result.video_path = video.device_path;
    result.serial_path = serial.device_path;
    result.usb_parent = complete_parents.front();
    result.bus = video.bus;
    return result;
}

std::vector<CherryVideoEndpoint> scan_cherry_video_endpoints() {
    std::vector<CherryVideoEndpoint> endpoints;
#ifdef __linux__
    DIR* directory = opendir("/sys/class/video4linux");
    if (!directory) {
        fprintf(stderr, "ERROR: cannot open /sys/class/video4linux: %s\n",
                strerror(errno));
        return endpoints;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strncmp(entry->d_name, "video", 5) != 0) continue;

        const std::string name = entry->d_name;
        UsbDeviceInfo usb;
        if (!find_usb_device("/sys/class/video4linux/" + name, &usb)) {
            continue;
        }

        CherryVideoEndpoint endpoint;
        endpoint.device_path = "/dev/" + name;
        endpoint.usb_parent = std::move(usb.parent);
        endpoint.vid = usb.vid;
        endpoint.pid = usb.pid;
        endpoint.bus = usb.bus;
        endpoint.supports_target =
            supports_cherry_target(endpoint.device_path);
        endpoints.push_back(std::move(endpoint));
    }
    closedir(directory);
#endif
    return endpoints;
}

std::vector<CherrySerialEndpoint> scan_cherry_serial_endpoints() {
    std::vector<CherrySerialEndpoint> endpoints;
#ifdef __linux__
    DIR* directory = opendir("/sys/class/tty");
    if (!directory) {
        fprintf(stderr, "ERROR: cannot open /sys/class/tty: %s\n",
                strerror(errno));
        return endpoints;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (strncmp(entry->d_name, "ttyACM", 6) != 0) continue;

        const std::string name = entry->d_name;
        UsbDeviceInfo usb;
        if (!find_usb_device("/sys/class/tty/" + name, &usb)) continue;

        CherrySerialEndpoint endpoint;
        endpoint.device_path = "/dev/" + name;
        endpoint.usb_parent = std::move(usb.parent);
        endpoint.vid = usb.vid;
        endpoint.pid = usb.pid;
        endpoint.bus = usb.bus;
        endpoints.push_back(std::move(endpoint));
    }
    closedir(directory);
#endif
    return endpoints;
}

CherryDiscoveryResult discover_cherry_device(const CherryDeviceMap& device_map) {
    return match_cherry_device(device_map, scan_cherry_video_endpoints(),
                               scan_cherry_serial_endpoints());
}
