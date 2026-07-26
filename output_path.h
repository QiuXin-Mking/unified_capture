#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <sys/stat.h>

inline constexpr const char kSdMountPath[] = "/media/usb0";
inline constexpr const char kSdCaptureRoot[] = "/media/usb0/capture";

inline bool is_sd_capture_path(const std::string& path) {
    const std::string root{kSdCaptureRoot};
    return path == root || path.rfind(root + "/", 0) == 0;
}

// All relative output names are rooted on the SD card. Absolute paths are
// returned unchanged so the caller can reject paths outside kSdCaptureRoot.
inline std::string capture_output_prefix(const std::string& requested) {
    if (!requested.empty()) {
        if (requested.front() == '/') return requested;
        return std::string(kSdCaptureRoot) + "/" + requested;
    }

    const time_t now = time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    char name[64];
    snprintf(name, sizeof(name), "record_%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(kSdCaptureRoot) + "/" + name;
}

inline bool is_sd_card_mounted() {
    struct stat mount_stat {};
    struct stat parent_stat {};
    return stat(kSdMountPath, &mount_stat) == 0 &&
           stat("/media", &parent_stat) == 0 &&
           mount_stat.st_dev != parent_stat.st_dev;
}
