#pragma once

#include "hardware/video/capture_control.h"
#include "hardware/video/device_discovery.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SessionOptions {
    bool use_imu;
    bool use_as5600;
    bool use_vive;
    bool use_h265;
};

class Sensor;
using ControlPump = std::function<void(int timeout_ms)>;

class SessionRunner {
public:
    SessionRunner(const CameraDiscoveryResult& cameras,
                  SessionOptions options,
                  std::atomic<bool>& session_running);
    ~SessionRunner();

    std::string make_session_dir(const std::string& prefix, int session_number) const;
    void run(const std::string& session_dir,
             int session_number,
             const ControlPump& pump);
    void wait_teardown();
    std::string cameras_json() const;
    void request_preview(std::string path);

private:
    CameraDiscoveryResult cameras_;
    SessionOptions options_;
    std::atomic<bool>& session_running_;
    VideoCaptureControl capture_control_;
    std::vector<std::unique_ptr<Sensor>> sensors_;
};
