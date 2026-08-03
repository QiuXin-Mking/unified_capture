#include "app/status_response.h"

namespace {

void append_escaped_json_string(std::string* output, const std::string& value) {
    output->push_back('"');
    for (char character : value) {
        switch (character) {
            case '\\': output->append("\\\\"); break;
            case '"': output->append("\\\""); break;
            case '\n': output->append("\\n"); break;
            case '\r': output->append("\\r"); break;
            case '\t': output->append("\\t"); break;
            default: output->push_back(character); break;
        }
    }
    output->push_back('"');
}

const char* json_bool(bool value) {
    return value ? "true" : "false";
}

}  // namespace

CaptureSensorStatus capture_sensor_status(ProductProfile profile,
                                          bool requested_imu,
                                          bool requested_as5600,
                                          bool detected_vive) {
    if (profile == ProductProfile::cherry) {
        return {true, false, false};
    }
    if (profile == ProductProfile::banana) {
        return {requested_imu, false, false};
    }
    return {requested_imu, requested_as5600, detected_vive};
}

std::string make_capture_status_json(const CaptureStatusResponse& status) {
    std::string json = "{\"ok\":true,\"product\":";
    append_escaped_json_string(&json, status.product);
    json += ",\"ready\":";
    json += json_bool(status.ready);
    json += ",\"degraded\":";
    json += json_bool(status.degraded);
    json += ",\"running\":";
    json += json_bool(status.running);
    json += ",\"session\":null,\"elapsed_ms\":" +
            std::to_string(status.elapsed_ms) + ",\"cameras\":{";
    for (std::size_t index = 0; index < status.cameras.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        append_escaped_json_string(&json, status.cameras[index].first);
        json.push_back(':');
        json += json_bool(status.cameras[index].second);
    }
    json += "},\"camera_errors\":[";
    for (std::size_t index = 0; index < status.camera_errors.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        append_escaped_json_string(&json, status.camera_errors[index]);
    }
    json += "],\"imu\":";
    json += json_bool(status.imu);
    json += ",\"as5600\":";
    json += json_bool(status.as5600);
    json += ",\"vive\":";
    json += json_bool(status.vive);
    json.push_back('}');
    return json;
}
