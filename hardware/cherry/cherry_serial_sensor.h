#pragma once

#include "hardware/cherry/cherry_protocol.h"
#include "hardware/cherry/cherry_start_control.h"
#include "hardware/common/sensor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cherry {

bool write_imu_jsonl(FILE* file, const ImuFrame& frame);
bool write_mag_jsonl(FILE* file, const MagFrame& frame);
bool write_frame_meta_jsonl(FILE* file, const FrameMeta& frame);

class SerialReadLoop {
public:
    using ReadStep = std::function<bool(int)>;

    SerialReadLoop(std::atomic<bool>& running, ReadStep read_step);
    ~SerialReadLoop();

    bool start();
    void wait_until_stopped();
    void stop_and_join();

private:
    std::atomic<bool>& running_;
    ReadStep read_step_;
    std::atomic<bool> stop_requested_{false};
    std::mutex mutex_;
    std::condition_variable stopped_;
    std::thread thread_;
    bool active_ = false;
};

class SerialLifecycleCoordinator {
public:
    bool after_ack(bool acknowledged,
                   const std::function<bool()>& start_reader,
                   const std::function<void()>& mark_ready) const;
    void before_stop(const std::function<void()>& stop_reader,
                     const std::function<void()>& send_stop) const;
};

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
    bool send_control_until(
        const std::vector<uint8_t>& bytes,
        std::chrono::steady_clock::time_point deadline);
    void drain_after_stop(std::chrono::steady_clock::time_point deadline);
    ReadResult read_once(int timeout_ms, bool* start_acknowledged = nullptr);
    bool process_frame(const cherry::Frame& frame,
                       bool* start_acknowledged);
    void fail_setup(const std::string& error);
    void close_resources();

    std::string tty_path_;
    std::string session_dir_;
    std::string output_dir_;
    CherryStartControl& start_control_;
    cherry::SerialLifecycleCoordinator lifecycle_;
    cherry::SerialReadLoop reader_;
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
