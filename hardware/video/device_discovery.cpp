#include "hardware/video/device_discovery.h"
#include "hardware/video/v4l2_device.h"
#include "hardware/wrist/wrist_discovery.h"
#include "hardware/wrist/wrist_profile.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint16_t kJhh2Vid = 0x1bcf;
constexpr uint16_t kJhh2Pid = 0x2d50;
constexpr uint16_t kSixVid = 0x1bcf;
constexpr uint16_t kSixPid = 0x2d51;

struct DiscoveredDevice {
    std::string path;       // /dev/videoN
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint32_t bus = 0;
    std::string product;    // iProduct string
};

// ── sysfs helpers ──

std::string read_sysfs_file(const std::string& path) {
    std::string result;
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return result;
    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
        // Trim trailing newline
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        result = buf;
    }
    fclose(fp);
    return result;
}

uint16_t read_sysfs_hex(const std::string& path) {
    std::string s = read_sysfs_file(path);
    if (s.empty()) return 0;
    return (uint16_t)strtoul(s.c_str(), nullptr, 16);
}

uint32_t read_sysfs_uint(const std::string& path) {
    std::string s = read_sysfs_file(path);
    if (s.empty()) return 0;
    return (uint32_t)strtoul(s.c_str(), nullptr, 10);
}

// Get the parent directory component from a path.
// e.g. "/a/b/c" → "/a/b"
std::string dirname(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

// ── V4L2 format enumeration (for banana wrist discovery) ──

bool enumerate_mjpeg_formats(const std::string& dev_path,
                              std::vector<WristVideoFormat>& out_formats) {
    int fd = ::open(dev_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG) {
            struct v4l2_frmsizeenum fsize = {};
            fsize.pixel_format = V4L2_PIX_FMT_MJPEG;

            while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0) {
                int w = 0, h = 0;
                if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    w = fsize.discrete.width;
                    h = fsize.discrete.height;
                } else {
                    fsize.index++;
                    continue;
                }

                struct v4l2_frmivalenum fival = {};
                fival.pixel_format = V4L2_PIX_FMT_MJPEG;
                fival.width  = (uint32_t)w;
                fival.height = (uint32_t)h;

                while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0) {
                    int fps = 0;
                    if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
                        fival.discrete.denominator > 0) {
                        fps = fival.discrete.numerator > 0
                            ? (int)(fival.discrete.denominator / fival.discrete.numerator)
                            : 0;
                    }
                    if (fps > 0) {
                        out_formats.push_back({true, w, h, fps});
                    }
                    fival.index++;
                }
                fsize.index++;
            }
        }
        fmtdesc.index++;
    }

    ::close(fd);
    return true;
}

// ── sysfs V4L2 device scan ──

// Skip metadata-only nodes (e.g. some UVC cameras expose two /dev/video
// nodes, only one of which advertises video formats).
static bool has_video_capture_formats(const std::string& dev_path) {
    int fd = ::open(dev_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct v4l2_capability cap = {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ::close(fd);
        return false;
    }

    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool has_format = (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0);
    ::close(fd);
    return has_format;
}

std::vector<DiscoveredDevice> scan_v4l2_devices() {
    std::vector<DiscoveredDevice> result;

    DIR* dir = opendir("/sys/class/video4linux");
    if (!dir) {
        fprintf(stderr, "ERROR: cannot open /sys/class/video4linux: %s\n",
                strerror(errno));
        return result;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "video", 5) != 0) continue;

        std::string v4l_path = "/sys/class/video4linux/" + std::string(entry->d_name);

        // Resolve symlink to real path:
        //   .../usbX/X-Y/X-Y.Z/X-Y.Z:W.W/video4linux/videoN
        char real[PATH_MAX];
        if (!realpath(v4l_path.c_str(), real)) continue;
        std::string resolved(real);

        // Walk up 3 levels: /video4linux/videoN → USB interface → USB device
        std::string iface_dir = dirname(dirname(resolved));  // strip /video4linux/videoN
        std::string usb_dev  = dirname(iface_dir);           // strip /USB_IFACE

        uint16_t vid = read_sysfs_hex(usb_dev + "/idVendor");
        uint16_t pid = read_sysfs_hex(usb_dev + "/idProduct");
        uint32_t bus = read_sysfs_uint(usb_dev + "/busnum");
        std::string product = read_sysfs_file(usb_dev + "/product");

        if (vid == 0) continue;  // Not a USB device

        // Skip metadata-only /dev/video nodes that do not expose any
        // capture formats. Many UVC cameras enumerate a second node for
        // metadata; the actual video node lists MJPEG/YUYV formats.
        std::string dev_node = "/dev/" + std::string(entry->d_name);
        if (!has_video_capture_formats(dev_node)) continue;

        DiscoveredDevice dev;
        dev.path    = "/dev/" + std::string(entry->d_name);
        dev.vid     = vid;
        dev.pid     = pid;
        dev.bus     = bus;
        dev.product = product;
        result.push_back(std::move(dev));
    }
    closedir(dir);
    return result;
}

// ── initial_result (mango profile defaults) ──

CameraDiscoveryResult initial_result() {
    CameraDiscoveryResult result;
    result.sixcam.enabled = true;
    result.jhh2 = {{
        {{"jhh2_left", kJhh2Vid, kJhh2Pid, 0, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true}, true},
        {{"jhh2_right", kJhh2Vid, kJhh2Pid, 1, 3840, 1200, 30, 16000000, 30,
          true, ImuOrientation::HORIZONTAL_TOP, true, true}, true},
    }};
    return result;
}

// ── discover_mango_cameras ──

CameraDiscoveryResult discover_mango_cameras() {
    CameraDiscoveryResult result = initial_result();

    std::vector<DiscoveredDevice> devices = scan_v4l2_devices();
    printf("V4L2: found %zu device(s)\n", devices.size());
    if (devices.empty()) return result;

    for (const auto& d : devices) {
        printf("  %s: %04x:%04x bus=%u product=\"%s\"\n",
               d.path.c_str(), d.vid, d.pid, d.bus, d.product.c_str());
    }

    uint32_t sixcam_bus = 0;

    // Find JHH04 (1bcf:2d51)
    for (const auto& d : devices) {
        if (d.vid == kSixVid && d.pid == kSixPid) {
            result.sixcam.jhh04_path = d.path;
            result.sixcam.enabled = true;
            sixcam_bus = d.bus;
            printf("  %-12s -> %s bus=%u  3104x480@30 IMU=Y (SixCam)\n",
                   "jhh04", d.path.c_str(), d.bus);
            result.active_count++;
            break;
        }
    }

    // Find JHH02 (1bcf:2d50, same bus as JHH04)
    if (result.sixcam.enabled && sixcam_bus > 0) {
        for (const auto& d : devices) {
            if (d.vid == kJhh2Vid && d.pid == kJhh2Pid && d.bus == sixcam_bus) {
                result.sixcam.jhh02_path = d.path;
                printf("  %-12s -> %s bus=%u  4000x1200@30 IMU=Y (SixCam)\n",
                       "jhh02", d.path.c_str(), d.bus);
                result.active_count++;
                break;
            }
        }
        if (result.sixcam.jhh02_path.empty()) {
            fprintf(stderr, "WARN: jhh02 not found on SixCam bus %u\n", sixcam_bus);
            result.sixcam.enabled = false;
            result.active_count--;
        }
    }

    // Assign remaining 1bcf:2d50 devices to independent JHH2 left/right
    int jhh2_index = 0;
    for (const auto& d : devices) {
        if (d.vid != kJhh2Vid || d.pid != kJhh2Pid) continue;
        if (d.bus == sixcam_bus) continue;  // Already assigned to sixcam

        if (jhh2_index >= static_cast<int>(result.jhh2.size())) break;

        auto& camera = result.jhh2[jhh2_index];
        camera.device_path = d.path;
        camera.enabled = true;
        printf("  %-12s -> %s bus=%u  %dx%d@%d IMU=%c\n",
               camera.config.name, d.path.c_str(), d.bus,
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
        if (camera.enabled && !camera.device_path.empty()) {
            result.active_count++;
        }
    }
    return result;
}

// ── discover_banana_cameras ──

CameraDiscoveryResult discover_banana_cameras(
    const ProductConfiguration& configuration) {
    CameraDiscoveryResult result;
    result.profile = ProductProfile::banana;

    std::vector<DiscoveredDevice> devices = scan_v4l2_devices();
    printf("V4L2: found %zu device(s)\n", devices.size());
    if (devices.empty()) {
        result.degraded = configuration.wrist.allow_missing_devices;
        return result;
    }

    for (const auto& d : devices) {
        printf("  %s: %04x:%04x bus=%u product=\"%s\"\n",
               d.path.c_str(), d.vid, d.pid, d.bus, d.product.c_str());
    }

    // Build wrist inventory
    std::vector<WristDeviceInfo> inventory;
    inventory.reserve(devices.size());
    for (const auto& d : devices) {
        WristDeviceInfo device;
        device.device_path = d.path;
        device.vid = d.vid;
        device.pid = d.pid;
        device.product = d.product;

        enumerate_mjpeg_formats(d.path, device.formats);

        printf("  %s: product=\"%s\" formats=%zu\n",
               d.path.c_str(), device.product.c_str(), device.formats.size());
        inventory.push_back(std::move(device));
    }

    WristDiscoveryResult wrist =
        match_wrist_cameras(configuration.wrist, inventory);
    for (std::size_t i = 0; i < result.wrist.size(); ++i) {
        result.wrist[i].config = wrist.cameras[i].config;
        result.wrist[i].enabled = wrist.cameras[i].available;
        result.wrist[i].device_path = wrist.cameras[i].device_path;
    }
    result.degraded = wrist.degraded;
    result.camera_errors = std::move(wrist.errors);
    result.active_count = wrist.active_count;

    // ── SixCam discovery (banana) ──
    if (configuration.sixcam_enabled) {
        uint32_t sixcam_bus = 0;

        // JHH04 (1bcf:2d51)
        for (const auto& d : devices) {
            if (d.vid == kSixVid && d.pid == kSixPid) {
                result.sixcam.jhh04_path = d.path;
                result.sixcam.enabled = true;
                sixcam_bus = d.bus;
                printf("  %-12s -> %s bus=%u (SixCam)\n",
                       "jhh04", d.path.c_str(), d.bus);
                result.active_count++;
                break;
            }
        }

        // JHH02 (1bcf:2d50, same bus)
        if (result.sixcam.enabled && sixcam_bus > 0) {
            for (const auto& d : devices) {
                if (d.vid == kJhh2Vid && d.pid == kJhh2Pid &&
                    d.bus == sixcam_bus) {
                    result.sixcam.jhh02_path = d.path;
                    printf("  %-12s -> %s bus=%u (SixCam)\n",
                           "jhh02", d.path.c_str(), d.bus);
                    result.active_count++;
                    break;
                }
            }
            if (result.sixcam.jhh02_path.empty()) {
                fprintf(stderr, "WARN: jhh02 not found on SixCam bus %u\n",
                        sixcam_bus);
                result.sixcam.enabled = false;
                result.active_count--;
                result.camera_errors.emplace_back(
                    "jhh02 not found on sixcam bus");
            }
        } else {
            result.sixcam.enabled = false;
            if (result.camera_errors.empty() ||
                configuration.wrist.allow_missing_devices) {
                result.camera_errors.emplace_back("jhh04 sixcam not found");
            }
        }
    }

    return result;
}

}  // namespace

// ── Public interface ──

void scan_devices() {
    std::vector<DiscoveredDevice> devices = scan_v4l2_devices();
    printf("Found %zu V4L2 device(s):\n", devices.size());
    for (const auto& d : devices) {
        printf("  %s: %04x:%04x bus=%u product=\"%s\"\n",
               d.path.c_str(), d.vid, d.pid, d.bus, d.product.c_str());
    }
}

CameraDiscoveryResult discover_cameras(const ProductConfiguration& configuration) {
    if (configuration.profile == ProductProfile::banana) {
        return discover_banana_cameras(configuration);
    }
    return discover_mango_cameras();
}
