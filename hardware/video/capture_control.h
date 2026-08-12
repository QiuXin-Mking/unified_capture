#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class VideoCaptureControl {
public:
    void reset_stream_start(int independent_jhh2_count, bool sixcam_jhh02_available) {
        jhh2_remaining = independent_jhh2_count + (sixcam_jhh02_available ? 1 : 0);
        jhh02_init_done = !sixcam_jhh02_available;
    }

    void mark_jhh02_started() {
        jhh02_init_done = true;
        decrement_remaining();
    }

    void mark_wrist_started() {
        decrement_remaining();
    }

    bool wrists_may_start() const {
        return jhh02_init_done.load();
    }

    bool jhh04_may_start() const {
        return jhh2_remaining.load() == 0;
    }

    void request_preview(std::string channel, std::string path) {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        if (channel.empty()) {
            legacy_preview_path_ = std::move(path);
            legacy_preview_pending_ = true;
            return;
        }
        preview_paths_[std::move(channel)] = std::move(path);
    }

    bool take_preview(const std::string& camera_name, std::string& path) {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        const auto request = preview_paths_.find(camera_name);
        if (request != preview_paths_.end()) {
            path = std::move(request->second);
            preview_paths_.erase(request);
            return true;
        }
        if (legacy_preview_pending_) {
            legacy_preview_pending_ = false;
            path = std::move(legacy_preview_path_);
            return true;
        }
        return false;
    }

    std::atomic<int> jhh2_remaining{0};
    std::atomic<bool> jhh02_init_done{false};

private:
    void decrement_remaining() {
        int current = jhh2_remaining.load();
        while (current > 0 &&
               !jhh2_remaining.compare_exchange_weak(current, current - 1)) {
        }
    }

    bool legacy_preview_pending_ = false;
    std::string legacy_preview_path_;
    std::unordered_map<std::string, std::string> preview_paths_;
    std::mutex preview_mutex_;
};
