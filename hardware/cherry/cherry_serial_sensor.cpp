#include "hardware/cherry/cherry_serial_sensor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr uint8_t kStreamMask = 0x07;
constexpr uint16_t kStartSequence = 1;
constexpr uint16_t kStopSequence = 2;

bool flush_json_line(FILE* file)
{
    return fputc('\n', file) != EOF && fflush(file) == 0;
}

bool write_imu_samples(FILE* file, const std::vector<cherry::ImuSample>& samples)
{
    for (size_t index = 0; index < samples.size(); ++index) {
        const cherry::ImuSample& sample = samples[index];
        if (index != 0 && fputc(',', file) == EOF) return false;
        if (fprintf(file,
                    "{\"x\":%" PRId16 ",\"y\":%" PRId16
                    ",\"z\":%" PRId16 ",\"temperature\":%" PRId32
                    ",\"pts_us\":%" PRIu64 "}",
                    sample.x, sample.y, sample.z, sample.temperature,
                    sample.pts_us) < 0) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace cherry {

SerialReadLoop::SerialReadLoop(std::atomic<bool>& running, ReadStep read_step)
    : running_(running), read_step_(std::move(read_step))
{
}

SerialReadLoop::~SerialReadLoop()
{
    stop_and_join();
}

bool SerialReadLoop::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || active_) return false;
    stop_requested_ = false;
    active_ = true;
    try {
        thread_ = std::thread([this] {
            while (running_ && !stop_requested_) {
                if (!read_step_(200)) {
                    running_ = false;
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> done_lock(mutex_);
                active_ = false;
            }
            stopped_.notify_all();
        });
    } catch (...) {
        active_ = false;
        return false;
    }
    return true;
}

void SerialReadLoop::wait_until_stopped()
{
    std::unique_lock<std::mutex> lock(mutex_);
    stopped_.wait(lock, [this] { return !active_; });
}

void SerialReadLoop::stop_and_join()
{
    stop_requested_ = true;
    if (thread_.joinable()) thread_.join();
}

bool write_imu_jsonl(FILE* file, const ImuFrame& frame)
{
    if (!file ||
        fprintf(file,
                "{\"generation\":%" PRIu32
                ",\"window_begin_pts_us\":%" PRIu64
                ",\"window_end_pts_us\":%" PRIu64
                ",\"gyro_samples\":[",
                frame.generation, frame.window_begin_pts_us,
                frame.window_end_pts_us) < 0 ||
        !write_imu_samples(file, frame.gyro) ||
        fputs("],\"acc_samples\":[", file) == EOF ||
        !write_imu_samples(file, frame.acc) ||
        fputs("]}", file) == EOF) {
        return false;
    }
    return flush_json_line(file);
}

bool write_mag_jsonl(FILE* file, const MagFrame& frame)
{
    if (!file ||
        fprintf(file, "{\"generation\":%" PRIu32 ",\"samples\":[",
                frame.generation) < 0) {
        return false;
    }
    for (size_t index = 0; index < frame.samples.size(); ++index) {
        const MagSample& sample = frame.samples[index];
        if ((index != 0 && fputc(',', file) == EOF) ||
            fprintf(file,
                    "{\"x_raw\":%" PRId32 ",\"y_raw\":%" PRId32
                    ",\"z_raw\":%" PRId32 ",\"tout_raw\":%u"
                    ",\"pts_us\":%" PRIu64 "}",
                    sample.x_raw, sample.y_raw, sample.z_raw,
                    static_cast<unsigned int>(sample.tout_raw),
                    sample.pts_us) < 0) {
            return false;
        }
    }
    return fputs("]}", file) != EOF && flush_json_line(file);
}

bool write_frame_meta_jsonl(FILE* file, const FrameMeta& frame)
{
    if (!file ||
        fprintf(file, "{\"generation\":%" PRIu32 ",\"samples\":[",
                frame.generation) < 0) {
        return false;
    }
    for (size_t index = 0; index < frame.samples.size(); ++index) {
        const FrameMetaSample& sample = frame.samples[index];
        if ((index != 0 && fputc(',', file) == EOF) ||
            fprintf(file,
                    "{\"sensor_idx\":%u,\"vi_pipe\":%u"
                    ",\"frame_id\":%" PRIu32
                    ",\"frame_pts_us\":%" PRIu64 "}",
                    static_cast<unsigned int>(sample.sensor_idx),
                    static_cast<unsigned int>(sample.vi_pipe),
                    sample.frame_id, sample.frame_pts_us) < 0) {
            return false;
        }
    }
    return fputs("]}", file) != EOF && flush_json_line(file);
}

} // namespace cherry

CherrySerialSensor::CherrySerialSensor(
    std::string tty_path, std::string sensor_name, std::string session_dir,
    std::atomic<bool>& running, CherryStartControl& start_control)
    : Sensor(std::move(sensor_name), running),
      tty_path_(std::move(tty_path)),
      session_dir_(std::move(session_dir)),
      start_control_(start_control),
      reader_(running_, [this](int timeout_ms) {
          const ReadResult result = read_once(timeout_ms);
          if (result == ReadResult::failed) {
              fprintf(stderr, "[%s] serial read failed: %s\n",
                      name_.c_str(), strerror(errno));
              return false;
          }
          return true;
      })
{
}

void CherrySerialSensor::setup()
{
    output_dir_ = session_dir_ + "/" + name_;
    if (mkdir_p(output_dir_.c_str(), 0755) != 0) {
        fail_setup("cannot create output directory " + output_dir_ + ": " +
                   strerror(errno));
        return;
    }
    if (!open_outputs()) return;

    fd_ = open(tty_path_.c_str(),
               O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        fail_setup("cannot open " + tty_path_ + ": " + strerror(errno));
        return;
    }
    if (!configure_port()) return;
    if (tcflush(fd_, TCIOFLUSH) < 0) {
        fail_setup("tcflush failed: " + std::string(strerror(errno)));
        return;
    }
    if (!send_control(cherry::encode_start(kStartSequence, kStreamMask),
                      2000)) {
        fail_setup("failed to send Cherry START: " +
                   std::string(strerror(errno)));
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(2000);
    bool acknowledged = false;
    while (!acknowledged) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            fail_setup("timed out waiting for Cherry START response");
            return;
        }
        const int timeout_ms = static_cast<int>(
            std::min<int64_t>(remaining.count(), 200));
        if (read_once(timeout_ms, &acknowledged) == ReadResult::failed) {
            fail_setup("serial read failed during START: " +
                       std::string(strerror(errno)));
            return;
        }
    }

    if (!reader_.start()) {
        fail_setup("cannot start Cherry serial reader thread");
        return;
    }
    initialized_ = true;
    start_control_.mark_ready();
    fprintf(stderr, "[%s] Cherry serial START acknowledged on %s\n",
            name_.c_str(), tty_path_.c_str());
}

void CherrySerialSensor::collect()
{
    if (!initialized_) return;
    reader_.wait_until_stopped();
}

void CherrySerialSensor::teardown()
{
    reader_.stop_and_join();
    if (fd_ >= 0) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        if (!send_control_until(cherry::encode_stop(kStopSequence), deadline)) {
            fprintf(stderr, "[%s] failed to send Cherry STOP: %s\n",
                    name_.c_str(), strerror(errno));
        }
        drain_after_stop(deadline);
    }
    close_resources();
    fprintf(stderr,
            "[%s] Cherry serial batches imu=%zu mag=%zu frame_meta=%zu "
            "parser_errors=%zu\n",
            name_.c_str(), imu_batches_, mag_batches_, frame_meta_batches_,
            parser_.error_count());
}

bool CherrySerialSensor::configure_port()
{
#ifndef B921600
    fail_setup("serial baud 921600 is unavailable on this platform");
    errno = ENOTSUP;
    return false;
#else
    struct termios attributes = {};
    if (tcgetattr(fd_, &attributes) < 0) {
        fail_setup("tcgetattr failed: " + std::string(strerror(errno)));
        return false;
    }
    cfmakeraw(&attributes);
    if (cfsetispeed(&attributes, B921600) < 0 ||
        cfsetospeed(&attributes, B921600) < 0) {
        fail_setup("cannot set serial baud 921600: " +
                   std::string(strerror(errno)));
        return false;
    }
    attributes.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    attributes.c_cflag |= CS8 | CLOCAL | CREAD;
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 1;
    if (tcsetattr(fd_, TCSANOW, &attributes) < 0) {
        fail_setup("tcsetattr failed: " + std::string(strerror(errno)));
        return false;
    }
    return true;
#endif
}

bool CherrySerialSensor::open_outputs()
{
    const auto open_one = [&](const char* filename, FILE** output) {
        const std::string path = output_dir_ + "/" + filename;
        const int fd = open(path.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            fail_setup("cannot create " + path + ": " + strerror(errno));
            return false;
        }
        const int descriptor_flags = fcntl(fd, F_GETFD);
        if (descriptor_flags < 0 ||
            fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
            const int saved_errno = errno;
            close(fd);
            fail_setup("cannot protect " + path + " from exec: " +
                       strerror(saved_errno));
            return false;
        }
        *output = fdopen(fd, "wb");
        if (!*output) {
            const int saved_errno = errno;
            close(fd);
            fail_setup("cannot create " + path + ": " +
                       strerror(saved_errno));
            return false;
        }
        return true;
    };
    return open_one("imu.jsonl", &imu_file_) &&
           open_one("mag.jsonl", &mag_file_) &&
           open_one("frame_meta.jsonl", &frame_meta_file_);
}

bool CherrySerialSensor::send_control(const std::vector<uint8_t>& bytes,
                                      int timeout_ms)
{
    return send_control_until(
        bytes, std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(timeout_ms));
}

bool CherrySerialSensor::send_control_until(
    const std::vector<uint8_t>& bytes,
    std::chrono::steady_clock::time_point deadline)
{
    if (fd_ < 0 || bytes.empty()) {
        errno = EINVAL;
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd_, bytes.data() + offset,
                                    bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        struct pollfd descriptor = {fd_, POLLOUT, 0};
        int status;
        do {
            status = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        } while (status < 0 && errno == EINTR);
        if (status <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            if (status == 0) errno = ETIMEDOUT;
            else if (status > 0) errno = EIO;
            return false;
        }
    }
    return true;
}

void CherrySerialSensor::drain_after_stop(
    std::chrono::steady_clock::time_point deadline)
{
    bool output_empty = false;
    while (std::chrono::steady_clock::now() < deadline) {
#ifdef TIOCOUTQ
        int pending_bytes = 0;
        if (ioctl(fd_, TIOCOUTQ, &pending_bytes) < 0) {
            fprintf(stderr, "[%s] TIOCOUTQ failed after STOP: %s\n",
                    name_.c_str(), strerror(errno));
            return;
        }
        output_empty = pending_bytes == 0;
#else
        output_empty = true;
#endif
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        const int read_timeout = static_cast<int>(
            std::min<int64_t>(remaining.count(), 20));
        const ReadResult result = read_once(read_timeout);
        if (result == ReadResult::failed) {
            fprintf(stderr, "[%s] serial drain failed after STOP: %s\n",
                    name_.c_str(), strerror(errno));
            return;
        }
        if (output_empty && result == ReadResult::timeout) return;
    }
    if (!output_empty) {
        fprintf(stderr,
                "[%s] timed out waiting for Cherry STOP bytes to leave tty\n",
                name_.c_str());
    }
}

CherrySerialSensor::ReadResult CherrySerialSensor::read_once(
    int timeout_ms, bool* start_acknowledged)
{
    struct pollfd descriptor = {fd_, POLLIN, 0};
    int status;
    do {
        status = poll(&descriptor, 1, timeout_ms);
    } while (status < 0 && errno == EINTR);
    if (status == 0) return ReadResult::timeout;
    if (status < 0) return ReadResult::failed;
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;
        return ReadResult::failed;
    }
    if (!(descriptor.revents & POLLIN)) return ReadResult::timeout;

    uint8_t buffer[4096];
    ssize_t count;
    do {
        count = read(fd_, buffer, sizeof(buffer));
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return ReadResult::timeout;
    }
    if (count < 0) return ReadResult::failed;
    if (count == 0) return ReadResult::timeout;

    const std::vector<cherry::Frame> frames = parser_.push(
        std::span<const uint8_t>(buffer, static_cast<size_t>(count)));
    if (parser_.error_count() != observed_parser_errors_) {
        fprintf(stderr, "[%s] Cherry parser discarded %zu corrupted frame(s)\n",
                name_.c_str(),
                parser_.error_count() - observed_parser_errors_);
        observed_parser_errors_ = parser_.error_count();
    }
    for (const cherry::Frame& frame : frames) {
        if (!process_frame(frame, start_acknowledged)) {
            errno = EPROTO;
            return ReadResult::failed;
        }
    }
    return ReadResult::ok;
}

bool CherrySerialSensor::process_frame(const cherry::Frame& frame,
                                       bool* start_acknowledged)
{
    if (frame.type == cherry::MessageType::start) {
        if (!start_acknowledged || frame.flags != 0x01 ||
            frame.sequence != kStartSequence || frame.payload.size() != 4 ||
            frame.payload[0] != kStreamMask || frame.payload[1] != 0 ||
            frame.payload[2] != 0 || frame.payload[3] != 0) {
            fprintf(stderr, "[%s] invalid Cherry START response\n",
                    name_.c_str());
            return false;
        }
        *start_acknowledged = true;
        return true;
    }
    if (frame.type == cherry::MessageType::error) {
        fprintf(stderr, "[%s] Cherry device returned ERROR for sequence %u\n",
                name_.c_str(), frame.sequence);
        return false;
    }
    if (frame.type == cherry::MessageType::imu_data) {
        const auto decoded = cherry::decode_imu(frame);
        if (!decoded || !cherry::write_imu_jsonl(imu_file_, *decoded)) {
            fprintf(stderr, "[%s] failed to write IMU JSONL\n", name_.c_str());
            return false;
        }
        ++imu_batches_;
    } else if (frame.type == cherry::MessageType::mag_data) {
        const auto decoded = cherry::decode_mag(frame);
        if (!decoded || !cherry::write_mag_jsonl(mag_file_, *decoded)) {
            fprintf(stderr, "[%s] failed to write MAG JSONL\n", name_.c_str());
            return false;
        }
        ++mag_batches_;
    } else if (frame.type == cherry::MessageType::frame_meta) {
        const auto decoded = cherry::decode_frame_meta(frame);
        if (!decoded ||
            !cherry::write_frame_meta_jsonl(frame_meta_file_, *decoded)) {
            fprintf(stderr, "[%s] failed to write FRAME_META JSONL\n",
                    name_.c_str());
            return false;
        }
        ++frame_meta_batches_;
    }
    return true;
}

void CherrySerialSensor::fail_setup(const std::string& error)
{
    fprintf(stderr, "[%s] Cherry serial setup failed: %s\n", name_.c_str(),
            error.c_str());
    start_control_.mark_failed(error);
    running_ = false;
}

void CherrySerialSensor::close_resources()
{
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    const auto close_file = [](FILE*& file) {
        if (file) {
            fclose(file);
            file = nullptr;
        }
    };
    close_file(imu_file_);
    close_file(mag_file_);
    close_file(frame_meta_file_);
}
