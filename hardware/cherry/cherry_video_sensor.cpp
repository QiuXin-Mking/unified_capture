#include "hardware/cherry/cherry_video_sensor.h"

#include "hardware/cherry/cherry_h264_writer.h"
#include "hardware/video/capture_pipeline.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

std::atomic<unsigned long> fifo_sequence{0};

std::string safe_fifo_component(std::string name)
{
    for (char& byte : name) {
        const bool safe = (byte >= 'a' && byte <= 'z') ||
                          (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' ||
                          byte == '-';
        if (!safe) byte = '_';
    }
    return name;
}

class StopOnWriterFailure {
public:
    StopOnWriterFailure(CherryH264Writer& writer,
                        std::atomic<bool>& running)
        : writer_(writer), running_(running)
    {
    }

    VideoFrameProcessResult process(const CompressedFrame& frame)
    {
        const VideoFrameProcessResult result = writer_.process(frame);
        if (result == VideoFrameProcessResult::encoder_failure) {
            running_ = false;
        }
        return result;
    }

    void finish()
    {
        writer_.finish();
        if (writer_.error()[0] != '\0') running_ = false;
    }

private:
    CherryH264Writer& writer_;
    std::atomic<bool>& running_;
};

} // namespace

CherryVideoSensor::CherryVideoSensor(
    const CameraConfig& config, std::string video_path,
    std::string session_dir, std::atomic<bool>& running,
    CherryStartControl& start_control)
    : Sensor(config.name, running),
      config_(config),
      video_path_(std::move(video_path)),
      session_dir_(std::move(session_dir)),
      start_control_(start_control)
{
}

void CherryVideoSensor::setup()
{
    output_dir_ = session_dir_ + "/" + name_;
    if (mkdir_p(output_dir_.c_str(), 0755) != 0) {
        fail_setup("cannot create output directory " + output_dir_ + ": " +
                   strerror(errno));
        return;
    }

    const CherryStartResult start_result =
        start_control_.wait(std::chrono::milliseconds(3000));
    if (!start_result.ready) {
        fail_setup(start_result.error);
        return;
    }
    if (!running_) {
        fail_setup("session stopped while waiting for Cherry serial START");
        return;
    }

    if (!device_.open(video_path_, config_.width, config_.height,
                      config_.fps, V4L2_PIX_FMT_H264)) {
        fail_setup("cannot open H.264 V4L2 device " + video_path_);
        return;
    }
    if (!open_outputs()) return;
    if (!running_) {
        fail_setup("session stopped before Cherry video STREAMON");
        return;
    }
    if (!device_.start_stream()) {
        fail_setup("V4L2 H.264 STREAMON failed");
        return;
    }

    stream_started_ = true;
    initialized_ = true;
    fprintf(stderr, "[%s] Cherry video setup OK (%dx%d H264 @ %dfps)\n",
            name_.c_str(), device_.actual_width(), device_.actual_height(),
            config_.fps);
}

void CherryVideoSensor::collect()
{
    if (!initialized_) return;

    CherryH264Writer writer(fifo_file_, metadata_file_);
    StopOnWriterFailure processor(writer, running_);
    stats_ = run_capture_pipeline(device_, running_, processor, 12);
    h264_bytes_ = writer.bytes();
    writer_error_ = writer.error();
    if (!writer_error_.empty()) {
        fprintf(stderr, "[%s] Cherry H.264 writer failed: %s\n",
                name_.c_str(), writer_error_.c_str());
    }
}

void CherryVideoSensor::teardown()
{
    if (stream_started_) {
        if (!device_.stop_stream()) {
            fprintf(stderr, "[%s] V4L2 H.264 STREAMOFF failed\n",
                    name_.c_str());
        }
        stream_started_ = false;
    }
    if (fifo_file_) {
        if (fclose(fifo_file_) != 0) {
            fprintf(stderr, "[%s] failed to close H.264 FIFO: %s\n",
                    name_.c_str(), strerror(errno));
        }
        fifo_file_ = nullptr;
    }
    if (metadata_file_) {
        if (fclose(metadata_file_) != 0) {
            fprintf(stderr, "[%s] failed to close video metadata: %s\n",
                    name_.c_str(), strerror(errno));
        }
        metadata_file_ = nullptr;
    }
    wait_for_ffmpeg();
    device_.close();
    if (!fifo_path_.empty() && unlink(fifo_path_.c_str()) < 0 &&
        errno != ENOENT) {
        fprintf(stderr, "[%s] failed to remove FIFO %s: %s\n",
                name_.c_str(), fifo_path_.c_str(), strerror(errno));
    }

    fprintf(stderr,
            "[%s] PIPELINE acquired=%llu processed=%llu gaps=%llu "
            "overflows=%llu encoder_failures=%llu h264_bytes=%zu\n",
            name_.c_str(),
            static_cast<unsigned long long>(stats_.acquired),
            static_cast<unsigned long long>(stats_.processed),
            static_cast<unsigned long long>(stats_.sequence_gaps),
            static_cast<unsigned long long>(stats_.queue_overflows),
            static_cast<unsigned long long>(stats_.encoder_failures),
            h264_bytes_);
}

bool CherryVideoSensor::open_outputs()
{
    const unsigned long sequence = fifo_sequence.fetch_add(1);
    fifo_path_ = "/tmp/cherry_h264_" + std::to_string(getpid()) + "_" +
                 safe_fifo_component(name_) + "_" +
                 std::to_string(sequence) + ".fifo";
    if (mkfifo(fifo_path_.c_str(), 0600) < 0) {
        fail_setup("cannot create H.264 FIFO " + fifo_path_ + ": " +
                   strerror(errno));
        return false;
    }

    const std::string output_path = output_dir_ + "/cherry_stereo.mkv";
    const std::string fps = std::to_string(config_.fps);
    ffmpeg_pid_ = fork();
    if (ffmpeg_pid_ < 0) {
        fail_setup("cannot fork ffmpeg: " + std::string(strerror(errno)));
        return false;
    }
    if (ffmpeg_pid_ == 0) {
        execlp("ffmpeg", "ffmpeg", "-y", "-hide_banner", "-loglevel",
               "error", "-f", "h264", "-r", fps.c_str(), "-i",
               fifo_path_.c_str(), "-c", "copy", output_path.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }
    if (!open_fifo_writer(3000)) {
        fail_setup("ffmpeg did not open H.264 FIFO: " +
                   std::string(strerror(errno)));
        return false;
    }

    const std::string metadata_path = output_dir_ + "/video_frames.jsonl";
    metadata_file_ = fopen(metadata_path.c_str(), "wb");
    if (!metadata_file_) {
        fail_setup("cannot create " + metadata_path + ": " +
                   strerror(errno));
        return false;
    }
    return true;
}

bool CherryVideoSensor::open_fifo_writer(int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = open(fifo_path_.c_str(),
                            O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            fifo_file_ = fdopen(fd, "wb");
            if (!fifo_file_) {
                const int saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return false;
            }
            std::string configure_error;
            if (!configure_cherry_fifo_stream(fifo_file_, configure_error)) {
                fprintf(stderr, "[%s] %s\n", name_.c_str(),
                        configure_error.c_str());
                fclose(fifo_file_);
                fifo_file_ = nullptr;
                errno = EIO;
                return false;
            }
            return true;
        }
        if (errno != ENXIO && errno != EINTR) return false;

        int child_status = 0;
        const pid_t child = waitpid(ffmpeg_pid_, &child_status, WNOHANG);
        if (child == ffmpeg_pid_) {
            ffmpeg_pid_ = 0;
            errno = EPIPE;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    errno = ETIMEDOUT;
    return false;
}

void CherryVideoSensor::fail_setup(const std::string& error)
{
    fprintf(stderr, "[%s] Cherry video setup failed: %s\n", name_.c_str(),
            error.c_str());
    running_ = false;
}

void CherryVideoSensor::wait_for_ffmpeg()
{
    if (ffmpeg_pid_ <= 0) return;

    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
        if (result == ffmpeg_pid_) {
            fprintf(stderr, "[%s] ffmpeg exited (%d)\n", name_.c_str(),
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            ffmpeg_pid_ = 0;
            return;
        }
        if (result < 0 && errno != EINTR) {
            fprintf(stderr, "[%s] waitpid(ffmpeg) failed: %s\n",
                    name_.c_str(), strerror(errno));
            ffmpeg_pid_ = 0;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    kill(ffmpeg_pid_, SIGTERM);
    for (int attempt = 0; attempt < 50; ++attempt) {
        const pid_t result = waitpid(ffmpeg_pid_, &status, WNOHANG);
        if (result == ffmpeg_pid_) {
            ffmpeg_pid_ = 0;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(ffmpeg_pid_, SIGKILL);
    while (waitpid(ffmpeg_pid_, &status, 0) < 0 && errno == EINTR) {
    }
    ffmpeg_pid_ = 0;
}
