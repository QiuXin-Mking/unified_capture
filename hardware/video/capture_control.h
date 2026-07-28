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
    bool preview_pending_ = false;
    std::string preview_path_;
    std::mutex preview_mutex_;
};
