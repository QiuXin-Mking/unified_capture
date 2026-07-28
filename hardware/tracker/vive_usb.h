#pragma once
/*
 * vive_usb.h — 自动 unbind/rebind usbfs (解决 VIVE Tracker LIBUSB_ERROR_BUSY)
 *
 * 内核有时会把 VIVE Tracker HID 接口绑定到 usbfs,
 * 这会阻止 libsurvive (通过 libusb) Claim 设备.
 * 在 survive_init() 之前扫描 sysfs 并自动 unbind.
 */

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

// 扫描所有 VIVE Tracker (28de:2300), 仅检测不 unbind
static int detect_vive_trackers() {
    int count = 0;
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return 0;
    struct dirent* entry;
    char path[512], buf[64];
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0'; else buf[n] = '\0';
        if (strcmp(buf, "28de") != 0) continue;
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0'; else buf[n] = '\0';
        if (strcmp(buf, "2300") != 0) continue;
        count++;
    }
    closedir(dir);
    return count;
}

// 扫描所有 VIVE Tracker (28de:2300), unbind usbfs, 返回 sysfs 设备名列表
static std::vector<std::string> unbind_all_vive_trackers() {
    std::vector<std::string> devnames;
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return devnames;

    struct dirent* entry;
    char path[512], buf[64];

    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (name[0] == '.') continue;

        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        else buf[n] = '\0';
        if (strcmp(buf, "28de") != 0) continue;

        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        else buf[n] = '\0';
        if (strcmp(buf, "2300") != 0) continue;

        printf("[vive] found tracker (28de:2300) at %s\n", name);
        devnames.push_back(name);

        // unbind usbfs
        for (int iface = 0; iface < 10; iface++) {
            snprintf(path, sizeof(path),
                     "/sys/bus/usb/devices/%s:1.%d/driver", name, iface);
            char link[256];
            n = readlink(path, link, sizeof(link) - 1);
            if (n <= 0) continue;
            link[n] = '\0';

            if (strstr(link, "usbfs")) {
                char ifname[256];
                snprintf(ifname, sizeof(ifname), "%s:1.%d", name, iface);
                printf("[vive] unbinding %s from usbfs\n", ifname);
                fd = open("/sys/bus/usb/drivers/usbfs/unbind", O_WRONLY);
                if (fd >= 0) {
                    ssize_t ignored = write(fd, ifname, strlen(ifname));
                    (void)ignored;
                    close(fd);
                }
            }
        }
    }
    closedir(dir);
    return devnames;
}

// 回绑所有 tracker 到 usbfs
static void rebind_all_usbfs(const std::vector<std::string>& devnames) {
    for (const auto& devname : devnames) {
        if (devname.empty()) continue;
        for (int iface = 0; iface < 10; iface++) {
            char ifname[256], path[512];
            snprintf(ifname, sizeof(ifname), "%s:1.%d", devname.c_str(), iface);
            snprintf(path, sizeof(path),
                     "/sys/bus/usb/devices/%s/driver", ifname);
            if (access(path, F_OK) == 0) continue;  // 已有驱动

            int fd = open("/sys/bus/usb/drivers/usbfs/bind", O_WRONLY);
            if (fd >= 0) {
                ssize_t ignored = write(fd, ifname, strlen(ifname));
                (void)ignored;
                close(fd);
            }
        }
    }
}
