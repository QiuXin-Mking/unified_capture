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

// 返回 Tracker 的 sysfs 设备名 (如 "2-1.2"), 未找到返回 null
static const char* unbind_usbfs_for_vive(std::string& out_devname) {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return nullptr;

    struct dirent* entry;
    char path[512], buf[64];
    const char* found = nullptr;

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
        out_devname = name;
        found = name;

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
        break;
    }
    closedir(dir);
    return found;
}

// 回绑 usbfs
static void rebind_usbfs(const std::string& devname) {
    if (devname.empty()) return;
    for (int iface = 0; iface < 10; iface++) {
        char ifname[256], path[512];
        snprintf(ifname, sizeof(ifname), "%s:1.%d", devname.c_str(), iface);
        // 只回绑当前未绑定驱动程序的接口
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
