#pragma once

#include "hardware/cherry/cherry_protocol.h"
#include "hardware/cherry/cherry_start_control.h"
#include "hardware/common/sensor.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace cherry {

bool write_imu_jsonl(FILE* file, const ImuFrame& frame);
bool write_mag_jsonl(FILE* file, const MagFrame& frame);
bool write_frame_meta_jsonl(FILE* file, const FrameMeta& frame);

} // namespace cherry

class CherrySerialSensor : public Sensor {
public:
    CherrySerialSensor(std::string tty_path, std::string sensor_name,
                       std::string session_dir, std::atomic<bool>& running,
                       CherryStartControl& start_control);

protected:
    void setup() override;
    void collect() override;
    void teardown() override;

private:
    enum class ReadResult {
        ok,
        timeout,
        failed,
    };

    bool configure_port();
    bool open_outputs();
    bool send_control(const std::vector<uint8_t>& bytes, int timeout_ms);
    ReadResult read_once(int timeout_ms, bool* start_acknowledged = nullptr);
    bool process_frame(const cherry::Frame& frame,
                       bool* start_acknowledged);
    void fail_setup(const std::string& error);
    void close_resources();

    std::string tty_path_;
    std::string session_dir_;
    std::string output_dir_;
    CherryStartControl& start_control_;
    int fd_ = -1;
    FILE* imu_file_ = nullptr;
    FILE* mag_file_ = nullptr;
    FILE* frame_meta_file_ = nullptr;
    cherry::StreamParser parser_;
    size_t observed_parser_errors_ = 0;
    size_t imu_batches_ = 0;
    size_t mag_batches_ = 0;
    size_t frame_meta_batches_ = 0;
    bool initialized_ = false;
};
