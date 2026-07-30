#pragma once

#include <atomic>
#include <mutex>
#include <string>
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

    void request_preview(std::string path) {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        preview_path_ = std::move(path);
        preview_pending_ = true;
    }

    bool take_preview(std::string& path) {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        if (!preview_pending_) {
            return false;
        }
        preview_pending_ = false;
        path = std::move(preview_path_);
        return true;
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

    bool preview_pending_ = false;
    std::string preview_path_;
    std::mutex preview_mutex_;
};
