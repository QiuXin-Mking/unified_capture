#include "app/status_response.h"

#include <cassert>
#include <string>

int main() {
    CaptureStatusResponse status{
        "banana", true, true, false, 0,
        {{"wrist_left", true}, {"wrist_right", false}},
        {"wrist_right: missing product JHHSW", "quoted: \"value\""},
        true, false, false};

    const std::string json = make_capture_status_json(status);
    assert(json.find("\"product\":\"banana\"") != std::string::npos);
    assert(json.find("\"product\":\"banana\"", json.find("\"product\":\"banana\"") + 1) == std::string::npos);
    assert(json.find("\"ready\":true") != std::string::npos);
    assert(json.find("\"degraded\":true") != std::string::npos);
    assert(json.find("\"wrist_left\":true") != std::string::npos);
    assert(json.find("\"wrist_right\":false") != std::string::npos);
    assert(json.find("\"camera_errors\":[") != std::string::npos);
    assert(json.find("quoted: \\\"value\\\"") != std::string::npos);

    const CaptureSensorStatus cherry_sensors = capture_sensor_status(
        ProductProfile::cherry, false, true, true);
    assert(cherry_sensors.imu);
    assert(!cherry_sensors.as5600);
    assert(!cherry_sensors.vive);

    const CaptureSensorStatus mango_sensors = capture_sensor_status(
        ProductProfile::mango, false, true, true);
    assert(!mango_sensors.imu);
    assert(mango_sensors.as5600);
    assert(mango_sensors.vive);

    const CaptureSensorStatus banana_sensors = capture_sensor_status(
        ProductProfile::banana, true, true, true);
    assert(banana_sensors.imu);
    assert(!banana_sensors.as5600);
    assert(!banana_sensors.vive);

    CaptureStatusResponse cherry_status{
        "cherry", true, false, false, 0,
        {{"cherry_stereo", true}}, {},
        cherry_sensors.imu, cherry_sensors.as5600, cherry_sensors.vive};
    const std::string cherry_json = make_capture_status_json(cherry_status);
    assert(cherry_json.find("\"product\":\"cherry\"") != std::string::npos);
    assert(cherry_json.find("\"cherry_stereo\":true") != std::string::npos);
    assert(cherry_json.find("\"imu\":true") != std::string::npos);
    assert(cherry_json.find("\"as5600\":false") != std::string::npos);
    assert(cherry_json.find("\"vive\":false") != std::string::npos);
}
