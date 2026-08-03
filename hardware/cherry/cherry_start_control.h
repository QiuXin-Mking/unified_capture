#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

struct CherryStartResult {
    bool ready = false;
    bool timed_out = false;
    std::string error;
};

class CherryStartControl {
public:
    void mark_ready()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::pending) return;
            state_ = State::ready;
        }
        changed_.notify_all();
    }

    void mark_failed(std::string error)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::pending) return;
            state_ = State::failed;
            error_ = std::move(error);
        }
        changed_.notify_all();
    }

    CherryStartResult wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] {
                return state_ != State::pending;
            })) {
            return {false, true,
                    "timed out waiting for Cherry serial START"};
        }
        if (state_ == State::ready) return {true, false, {}};
        return {false, false, error_};
    }

private:
    enum class State {
        pending,
        ready,
        failed,
    };

    std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::pending;
    std::string error_;
};
