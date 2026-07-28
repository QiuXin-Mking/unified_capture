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
}
