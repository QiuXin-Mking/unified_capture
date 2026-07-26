#pragma once

#include "../../sensor.h"
#include "resample_grid.h"
#include "libsurvive/survive.h"

#include <algorithm>
#include <unordered_map>

// Pose-only libsurvive collector. Each tracker is resampled on its own
// 48 MHz timecode clock; different trackers must never share one grid.
class ViveTrackerSensor : public Sensor {
public:
    ViveTrackerSensor(const std::string& session_dir, int,
                      const std::string&, std::atomic<bool>& running)
        : Sensor("vive_tracker", running), session_dir_(session_dir) {}

protected:
    void setup() override {
        char path[512];
        snprintf(path, sizeof(path), "%s/tracker_raw.jsonl", session_dir_.c_str());
        raw_file_ = fopen(path, "w");
        if (!raw_file_) { perror(path); return; }
        char program[] = "vive_tracker";
        char* argv[] = {program, nullptr};
        ctx_ = survive_init(1, argv);
        if (!ctx_) { fprintf(stderr, "[vive] survive_init failed\n"); return; }
        instance_ = this;
        survive_install_pose_fn(ctx_, pose_callback);
        ready_ = true;
        fprintf(stderr, "[vive] pose-only capture; per-device resample=100Hz\n");
    }

    void collect() override {
        while (ready_ && running_) {
            if (survive_poll(ctx_) != 0) break;
            usleep(2000);
        }
    }

    void teardown() override {
        if (!records_.empty()) write_resampled();
        if (raw_file_) { fflush(raw_file_); fclose(raw_file_); raw_file_ = nullptr; }
        if (ctx_) { survive_close(ctx_); ctx_ = nullptr; }
        instance_ = nullptr;
    }

private:
    struct Record { uint64_t timecode, ts_us; float pose[7]; };
    static constexpr uint64_t k100HzInterval = 480000;
    std::string session_dir_;
    SurviveContext* ctx_ = nullptr;
    FILE* raw_file_ = nullptr;
    bool ready_ = false;
    std::unordered_map<std::string, std::vector<Record>> records_;
    static inline ViveTrackerSensor* instance_ = nullptr;

    static void pose_callback(SurviveObject* so, survive_long_timecode tc,
                              const SurvivePose* pose) {
        if (!instance_ || !instance_->raw_file_ || !so || !pose) return;
        const char* name = so->codename ? so->codename : "unknown";
        const uint64_t ts = elapsed_us();
        fprintf(instance_->raw_file_, "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\",\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n", (unsigned long long)ts, (unsigned long long)tc, name, pose->Pos[0], pose->Pos[1], pose->Pos[2], pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);
        instance_->records_[name].push_back({tc, ts, {(float)pose->Pos[0], (float)pose->Pos[1], (float)pose->Pos[2], (float)pose->Rot[0], (float)pose->Rot[1], (float)pose->Rot[2], (float)pose->Rot[3]}});
    }

    static const Record& nearest(const std::vector<Record>& rows, uint64_t tc) {
        auto it = std::lower_bound(rows.begin(), rows.end(), tc, [](const Record& r, uint64_t value) { return r.timecode < value; });
        if (it == rows.begin()) return *it;
        if (it == rows.end()) return rows.back();
        return tc - (it - 1)->timecode <= it->timecode - tc ? *(it - 1) : *it;
    }

    void write_resampled() {
        char path[512]; snprintf(path, sizeof(path), "%s/tracker.jsonl", session_dir_.c_str());
        FILE* output = fopen(path, "w"); if (!output) { perror(path); return; }
        uint64_t count = 0;
        for (auto& [name, rows] : records_) {
            std::sort(rows.begin(), rows.end(), [](const Record& a, const Record& b) { return a.timecode < b.timecode; });
            if (rows.size() < 2) continue;
            const uint64_t first = rows.front().timecode, first_ts = rows.front().ts_us;
            for (uint64_t tc : make_resample_grid(first, rows.back().timecode, k100HzInterval)) {
                const Record& r = nearest(rows, tc);
                const uint64_t ts = first_ts + (tc - first) / 48;
                fprintf(output, "{\"ts_us\":%llu,\"timecode\":%llu,\"codename\":\"%s\",\"method\":\"nearest\",\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"qw\":%.6f,\"qx\":%.6f,\"qy\":%.6f,\"qz\":%.6f}\n", (unsigned long long)ts, (unsigned long long)tc, name.c_str(), r.pose[0], r.pose[1], r.pose[2], r.pose[3], r.pose[4], r.pose[5], r.pose[6]);
                count++;
            }
        }
        fclose(output);
        fprintf(stderr, "[vive] wrote %llu per-device 100Hz frames: %s\n", (unsigned long long)count, path);
    }
};
