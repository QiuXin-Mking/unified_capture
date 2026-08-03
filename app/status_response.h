#pragma once

#include "core/product_config.h"

#include <string>
#include <utility>
#include <vector>

struct CaptureStatusResponse {
    std::string product;
    bool ready = false;
    bool degraded = false;
    bool running = false;
    long elapsed_ms = 0;
    std::vector<std::pair<std::string, bool>> cameras;
    std::vector<std::string> camera_errors;
    bool imu = false;
    bool as5600 = false;
    bool vive = false;
};

struct CaptureSensorStatus {
    bool imu = false;
    bool as5600 = false;
    bool vive = false;
};

std::string make_capture_status_json(const CaptureStatusResponse& status);
CaptureSensorStatus capture_sensor_status(ProductProfile profile,
                                          bool requested_imu,
                                          bool requested_as5600,
                                          bool detected_vive);
