/*
 * test_resample.cpp — ViveTrackerSensor 独立系统测试
 *
 * 编译:
 *   g++ -std=c++20 -O2 -g -fPIC \
 *       -I/root/projects/libsurvive/include \
 *       -I/root/projects/libsurvive/include/libsurvive \
 *       -I/root/projects/libsurvive/redist \
 *       -I/root/projects/libsurvive/libs/cnmatrix/include \
 *       -I/root/projects/libsurvive/libs/cnkalman/src \
 *       -L/root/projects/libsurvive/bin \
 *       -Wl,-rpath,/root/projects/libsurvive/bin \
 *       test_resample.cpp \
 *       -lsurvive -lpthread -lrt -ldl -lm -lz \
 *       -o test_resample
 *
 * 用法:
 *   ./test_resample [duration_sec] [target_hz] [output_dir]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/stat.h>
#include "libsurvive/survive.h"

// ============================================================
// 位姿缓冲记录
// ============================================================
struct PoseRec {
    uint64_t tc;
    char     dev[8];
    float    x, y, z, qw, qx, qy, qz;
};

// ============================================================
// 最近邻搜索
// ============================================================
static const PoseRec* find_nearest(const std::vector<PoseRec>& poses,
                                    uint64_t target) {
    if (poses.empty()) return nullptr;
    auto it = std::lower_bound(poses.begin(), poses.end(), target,
        [](const PoseRec& r, uint64_t t) { return r.tc < t; });
    if (it == poses.begin()) return &poses.front();
    if (it == poses.end())   return &poses.back();
    const auto& a = *(it - 1);
    const auto& b = *it;
    return (target - a.tc <= b.tc - target) ? &a : &b;
}

// ============================================================
// 全局状态
// ============================================================
static struct timespec        g_t0;
static std::atomic<bool>      g_running{true};
static SurviveContext*        g_ctx = nullptr;
static FILE*                  g_fp_raw = nullptr;
static std::vector<PoseRec>   g_buffer;
static uint64_t               g_pose_count = 0;
static double                 g_target_hz = 100.0;
static uint64_t               g_interval  = 480000;  // 48MHz / 100Hz
static bool                   g_use_interp = false;

// ---- 时间工具 ----
static uint64_t elapsed_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - g_t0.tv_sec) * 1000000ULL +
           (uint64_t)(now.tv_nsec - g_t0.tv_nsec) / 1000ULL;
}

// ---- 信号 ----
static void sig_handler(int) { g_running = false; }

// ---- 位姿回调 ----
static void pose_cb(SurviveObject* so, uint64_t timecode,
                    const SurvivePose* pose) {
    if (!so || !pose || !g_fp_raw) return;
    g_pose_count++;

    uint64_t ts = elapsed_us();
    const char* name = so->codename ? so->codename : "unk";

    fprintf(g_fp_raw,
            "{\"ts_us\":%llu,\"tc\":%llu,\"dev\":\"%s\","
            "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,"
            "\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n",
            (unsigned long long)ts, (unsigned long long)timecode, name,
            pose->Pos[0], pose->Pos[1], pose->Pos[2],
            pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);

    // 缓冲
    PoseRec r;
    r.tc = timecode;
    strncpy(r.dev, name, sizeof(r.dev) - 1);
    r.dev[sizeof(r.dev) - 1] = '\0';
    r.x = pose->Pos[0]; r.y = pose->Pos[1]; r.z = pose->Pos[2];
    r.qw = pose->Rot[0]; r.qx = pose->Rot[1];
    r.qy = pose->Rot[2]; r.qz = pose->Rot[3];
    g_buffer.push_back(r);

    if (g_pose_count % 100 == 0) fflush(g_fp_raw);
}

// ---- 离线重采样 ----
static void resample_and_write(const char* outpath) {
    if (g_buffer.empty()) {
        fprintf(stderr, "缓冲为空\n"); return;
    }

    // 分组 + 排序
    struct DevSeries {
        std::string name;
        std::vector<PoseRec> poses;
        uint64_t t_min = UINT64_MAX, t_max = 0;
    };
    std::vector<DevSeries> devs;
    {
        std::unordered_map<std::string, DevSeries> map;
        for (auto& r : g_buffer) {
            auto& ds = map[r.dev];
            ds.name = r.dev;
            ds.poses.push_back(r);
            if (r.tc < ds.t_min) ds.t_min = r.tc;
            if (r.tc > ds.t_max) ds.t_max = r.tc;
        }
        for (auto& kv : map) {
            auto& d = kv.second;
            std::sort(d.poses.begin(), d.poses.end(),
                [](const PoseRec& a, const PoseRec& b) {
                    return a.tc < b.tc; });
            devs.push_back(std::move(d));
        }
    }
    std::sort(devs.begin(), devs.end(),
        [](const DevSeries& a, const DevSeries& b) {
            return a.name < b.name; });

    // 全局时间范围
    uint64_t t_min = UINT64_MAX, t_max = 0;
    for (auto& d : devs) {
        if (d.t_min < t_min) t_min = d.t_min;
        if (d.t_max > t_max) t_max = d.t_max;
    }

    const char* method = g_use_interp ? "lerp" : "nearest";
    FILE* fp = fopen(outpath, "w");
    if (!fp) { perror(outpath); return; }

    uint64_t total = 0, skipped = 0;
    for (uint64_t t = t_min + g_interval; t <= t_max; t += g_interval) {
        fprintf(fp, "{\"tc\":%llu,\"method\":\"%s\"",
                (unsigned long long)t, method);
        bool ok = true;
        for (auto& d : devs) {
            auto* r = find_nearest(d.poses, t);
            if (r) {
                fprintf(fp,
                    ",\"%s_x\":%.6f,\"%s_y\":%.6f,\"%s_z\":%.6f,"
                    "\"%s_qw\":%.6f,\"%s_qx\":%.6f,"
                    "\"%s_qy\":%.6f,\"%s_qz\":%.6f",
                    d.name.c_str(), r->x, d.name.c_str(), r->y,
                    d.name.c_str(), r->z, d.name.c_str(), r->qw,
                    d.name.c_str(), r->qx, d.name.c_str(), r->qy,
                    d.name.c_str(), r->qz);
            } else { ok = false; break; }
        }
        fprintf(fp, "}\n");
        ok ? total++ : skipped++;
    }
    fclose(fp);

    double dur = (t_max - t_min) / 48000000.0;
    printf("\n═══ 重采样结果 ═══\n");
    printf("设备: %zu 个\n", devs.size());
    for (auto& d : devs)
        printf("  %-8s %zu 条  %.1fs\n",
               d.name.c_str(), d.poses.size(),
               (d.t_max - d.t_min) / 48000000.0);
    printf("目标: %.0f Hz (%s)\n", g_target_hz, method);
    printf("输出: %llu 帧 / %.1fs = %.1f Hz (跳过 %llu)\n",
           (unsigned long long)total, dur, total / dur,
           (unsigned long long)skipped);
    printf("文件: %s\n", outpath);
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    int   duration   = (argc > 1) ? atoi(argv[1]) : 15;
    g_target_hz      = (argc > 2) ? atof(argv[2]) : 100.0;
    const char* outdir = (argc > 3) ? argv[3] : "/tmp/vive_test";

    g_interval = (uint64_t)(48000000.0 / g_target_hz);
    mkdir(outdir, 0755);

    printf("══════ VIVE Tracker 系统测试 ══════\n");
    printf("采集: %ds | 重采样: %.0f Hz | 输出: %s\n\n",
           duration, g_target_hz, outdir);
    fflush(stdout);

    // 文件
    char path[256];
    snprintf(path, sizeof(path), "%s/tracker_raw.jsonl", outdir);
    g_fp_raw = fopen(path, "w");
    if (!g_fp_raw) { perror(path); return 1; }

    // 初始化时钟
    clock_gettime(CLOCK_MONOTONIC, &g_t0);

    // survive_init
    const char* s_args[] = {
        "test_resample", "-l", "2",
        "--force-calibrate", "--force-ootx", nullptr
    };
    g_ctx = survive_init(5, const_cast<char**>(s_args));
    if (!g_ctx) {
        fprintf(stderr, "survive_init 失败\n"); return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    survive_install_pose_fn(g_ctx, pose_cb);

    // ★ 采集
    printf("[collect] 开始 %ds ...\n", duration);
    time_t t0 = time(nullptr);

    while (g_running && survive_poll(g_ctx) == 0) {
        time_t elapsed = time(nullptr) - t0;
        if (elapsed >= duration) break;
        if (elapsed > 0 && elapsed % 5 == 0) {
            static time_t last = 0;
            if (elapsed != last) {
                printf("  [%lds] %llu poses, %zu buffered\n",
                       elapsed, (unsigned long long)g_pose_count,
                       g_buffer.size());
                last = elapsed;
            }
        }
        usleep(2000);
    }

    printf("[collect] done: %llu poses, %zu buffered\n",
           (unsigned long long)g_pose_count, g_buffer.size());

    // 关闭 libsurvive
    survive_close(g_ctx);
    g_ctx = nullptr;
    fclose(g_fp_raw);
    g_fp_raw = nullptr;

    // ★ 离线重采样
    snprintf(path, sizeof(path), "%s/tracker.jsonl", outdir);
    resample_and_write(path);

    printf("\n═══ 完成 ═══\n");
    printf("原始: %s/tracker_raw.jsonl\n", outdir);
    printf("重采样: %s/tracker.jsonl\n", outdir);
    return 0;
}
