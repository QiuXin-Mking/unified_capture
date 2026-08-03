#pragma once
/*
 * v4l2_device.h — Pure Linux V4L2/UVC device abstraction
 *
 * Replaces Nori Xvision SDK for MJPEG camera capture.
 * Uses mmap buffer mode with non-blocking DQBUF (pull model).
 *
 * Usage:
 *   V4l2Device dev;
 *   dev.open("/dev/video0", 3840, 1200, 30);
 *   dev.start_stream();
 *   while (running) {
 *       size_t len;
 *       const uint8_t* mjpg = dev.dequeue_frame(len);
 *       if (!mjpg) { usleep(1000); continue; }   // EAGAIN
 *       // ... process mjpg[len] ...
 *       dev.requeue_frame();
 *   }
 *   dev.stop_stream();
 *   dev.close();
 */

#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "hardware/video/v4l2_frame_view.h"
#include "hardware/video/v4l2_format_validation.h"

struct V4l2Buffer {
    void*  start  = nullptr;
    size_t length = 0;
};

class V4l2Device {
public:
    static constexpr unsigned int kNumBuffers = 4;

    V4l2Device() = default;
    ~V4l2Device() { close(); }

    // Non-copyable, movable (required for SixCamChannel storage in std::array)
    V4l2Device(const V4l2Device&) = delete;
    V4l2Device& operator=(const V4l2Device&) = delete;

    V4l2Device(V4l2Device&& other) noexcept
        : fd_(other.fd_)
        , buffers_(std::move(other.buffers_))
        , dequeued_index_(other.dequeued_index_)
        , dequeued_len_(other.dequeued_len_)
        , actual_width_(other.actual_width_)
        , actual_height_(other.actual_height_)
        , fatal_error_(other.fatal_error_) {
        other.fd_ = -1;
    }

    V4l2Device& operator=(V4l2Device&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            buffers_ = std::move(other.buffers_);
            dequeued_index_ = other.dequeued_index_;
            dequeued_len_ = other.dequeued_len_;
            actual_width_ = other.actual_width_;
            actual_height_ = other.actual_height_;
            fatal_error_ = other.fatal_error_;
            other.fd_ = -1;
        }
        return *this;
    }

    // ── open: fd, S_FMT, S_PARM fps, REQBUFS mmap, QUERYBUF+mmap ──
    bool open(const std::string& path, int width, int height, int fps,
              uint32_t pixel_format = V4L2_PIX_FMT_MJPEG) {
        fatal_error_ = false;
        fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            fprintf(stderr, "[v4l2] open(%s) failed: %s\n", path.c_str(), strerror(errno));
            return false;
        }

        struct v4l2_capability cap = {};
        if (xioctl(VIDIOC_QUERYCAP, &cap) < 0) {
            fprintf(stderr, "[v4l2] QUERYCAP failed: %s\n", strerror(errno));
            close();
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            fprintf(stderr, "[v4l2] %s: not a capture device\n", path.c_str());
            close();
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            fprintf(stderr, "[v4l2] %s: no streaming support\n", path.c_str());
            close();
            return false;
        }

        // Set and verify the exact capture format requested by the caller.
        struct v4l2_format fmt = {};
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = (uint32_t)width;
        fmt.fmt.pix.height      = (uint32_t)height;
        fmt.fmt.pix.pixelformat = pixel_format;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;

        if (xioctl(VIDIOC_S_FMT, &fmt) < 0) {
            char requested_fourcc[5];
            fourcc_string(pixel_format, requested_fourcc);
            fprintf(stderr, "[v4l2] S_FMT %s failed: %s\n",
                    requested_fourcc, strerror(errno));
            close();
            return false;
        }
        actual_width_  = (int)fmt.fmt.pix.width;
        actual_height_ = (int)fmt.fmt.pix.height;
        char requested_fourcc[5];
        char selected_fourcc[5];
        fourcc_string(pixel_format, requested_fourcc);
        fourcc_string(fmt.fmt.pix.pixelformat, selected_fourcc);
        if (!validate_v4l2_selected_format(
                pixel_format, width, height,
                fmt.fmt.pix.pixelformat, actual_width_, actual_height_,
                [&] {
                    fprintf(stderr,
                            "[v4l2] format mismatch: requested %dx%d %s, "
                            "driver selected %dx%d %s\n",
                            width, height, requested_fourcc,
                            actual_width_, actual_height_, selected_fourcc);
                    close();
                })) {
            return false;
        }

        // Set framerate
        struct v4l2_streamparm parm = {};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = (uint32_t)fps;
        if (xioctl(VIDIOC_S_PARM, &parm) < 0) {
            fprintf(stderr, "[v4l2] S_PARM %dfps: %s (non-fatal)\n", fps, strerror(errno));
        }

        // Request mmap buffers
        struct v4l2_requestbuffers req = {};
        req.count  = kNumBuffers;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "[v4l2] REQBUFS failed: %s\n", strerror(errno));
            close();
            return false;
        }
        if (req.count < 2) {
            fprintf(stderr, "[v4l2] insufficient buffers: %u\n", req.count);
            close();
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; i++) {
            struct v4l2_buffer buf = {};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
                fprintf(stderr, "[v4l2] QUERYBUF[%u] failed: %s\n", i, strerror(errno));
                close();
                return false;
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                fprintf(stderr, "[v4l2] mmap[%u] failed: %s\n", i, strerror(errno));
                close();
                return false;
            }
        }

        fprintf(stderr, "[v4l2] %s opened: %dx%d %s @ %dfps, %zu bufs\n",
                path.c_str(), actual_width_, actual_height_, selected_fourcc,
                fps, buffers_.size());
        return true;
    }

    // ── start_stream: QBUF all → STREAMON ──
    bool start_stream() {
        for (uint32_t i = 0; i < buffers_.size(); i++) {
            struct v4l2_buffer buf = {};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            if (xioctl(VIDIOC_QBUF, &buf) < 0) {
                fatal_error_ = true;
                fprintf(stderr, "[v4l2] QBUF[%u] failed: %s\n", i, strerror(errno));
                return false;
            }
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMON, &type) < 0) {
            fatal_error_ = true;
            fprintf(stderr, "[v4l2] STREAMON failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    // ── dequeue_frame: non-blocking DQBUF with driver metadata ──
    bool dequeue_frame(V4l2FrameView& frame) {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return false;
            fatal_error_ = true;
            static int dq_errors = 0;
            if (dq_errors++ < 3) {
                fprintf(stderr, "[v4l2] DQBUF error: %s\n", strerror(errno));
            }
            return false;
        }
        if (buf.index >= buffers_.size()) {
            fatal_error_ = true;
            fprintf(stderr, "[v4l2] DQBUF invalid index: %u (count=%zu)\n",
                    buf.index, buffers_.size());
            return false;
        }
        if (buf.bytesused > buffers_[buf.index].length) {
            fatal_error_ = true;
            fprintf(stderr, "[v4l2] DQBUF invalid bytesused: %u (capacity=%zu)\n",
                    buf.bytesused, buffers_[buf.index].length);
            dequeued_index_ = buf.index;
            requeue_frame();
            return false;
        }
        dequeued_index_ = buf.index;
        dequeued_len_   = buf.bytesused;
        frame.data = static_cast<const uint8_t*>(buffers_[dequeued_index_].start);
        frame.size = dequeued_len_;
        frame.sequence = buf.sequence;
        frame.timestamp_us =
            static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000ULL +
            static_cast<uint64_t>(buf.timestamp.tv_usec);
        return true;
    }

    // Compatibility wrapper for existing synchronous pipelines.
    uint8_t* dequeue_frame(size_t& len) {
        V4l2FrameView frame;
        if (!dequeue_frame(frame)) {
            return nullptr;
        }
        len = frame.size;
        return const_cast<uint8_t*>(frame.data);
    }

    // ── requeue_frame: QBUF last dequeued index ──
    bool requeue_frame() {
        struct v4l2_buffer buf = {};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = dequeued_index_;
        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            fatal_error_ = true;
            fprintf(stderr, "[v4l2] QBUF requeue failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    // ── stop_stream: STREAMOFF ──
    bool stop_stream() {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMOFF, &type) < 0) {
            fprintf(stderr, "[v4l2] STREAMOFF failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    // ── close: munmap + close fd (idempotent) ──
    void close() {
        if (fd_ < 0) return;
        for (auto& b : buffers_) {
            if (b.start) { munmap(b.start, b.length); b.start = nullptr; }
        }
        buffers_.clear();
        ::close(fd_);
        fd_ = -1;
    }

    int actual_width()  const { return actual_width_; }
    int actual_height() const { return actual_height_; }
    bool fatal_error() const { return fatal_error_; }

    // Wait until a buffer is ready (or timeout_ms expires).
    // Returns true if data is available, false on timeout/error.
    bool wait_for_frame(int timeout_ms) {
        struct pollfd pfd = {};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int result;
        do {
            result = ::poll(&pfd, 1, timeout_ms);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            fatal_error_ = true;
            return false;
        }
        if (result == 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fatal_error_ = true;
            return false;
        }
        return (pfd.revents & POLLIN) != 0;
    }

private:
    static void fourcc_string(uint32_t fourcc, char text[5]) {
        for (unsigned int i = 0; i < 4; ++i) {
            const unsigned char byte =
                static_cast<unsigned char>((fourcc >> (8 * i)) & 0xff);
            text[i] = byte >= 0x20 && byte <= 0x7e
                          ? static_cast<char>(byte)
                          : '?';
        }
        text[4] = '\0';
    }

    int xioctl(unsigned long request, void* arg) {
        int r;
        do { r = ioctl(fd_, request, arg); } while (r == -1 && errno == EINTR);
        return r;
    }

    int fd_ = -1;
    std::vector<V4l2Buffer> buffers_;
    uint32_t dequeued_index_ = 0;
    size_t   dequeued_len_   = 0;
    int actual_width_  = 0;
    int actual_height_ = 0;
    bool fatal_error_ = false;
};
