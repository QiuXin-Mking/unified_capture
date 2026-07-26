#pragma once
/*
 * time_utils.h — 统一时间工具 + 文件工具 (所有 Sensor 共用)
 */

#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

// ============================================================
// 统一时间纪元
// ============================================================
extern struct timespec g_t0;

static inline uint64_t elapsed_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec  = (int64_t)(now.tv_sec  - g_t0.tv_sec);
    int64_t nsec = (int64_t)(now.tv_nsec - g_t0.tv_nsec);
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (uint64_t)(sec * 1000000LL + nsec / 1000LL);
}

// ============================================================
// 递归创建目录 (等效 mkdir -p)
// ============================================================
static inline int mkdir_p(const char* path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st {};
        if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}
