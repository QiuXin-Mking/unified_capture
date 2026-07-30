// v4l2_capture.h — Minimal V4L2 wrapper supporting YUYV + MJPEG
//
// Usage:
//   V4l2Capture cap;
//   cap.open("/dev/video0", 1440, 960, 30, V4L2_PIX_FMT_YUYV);
//   cap.start_stream();
//   while (running) {
//       if (cap.wait_frame(100)) {
//           const uint8_t* data; size_t len;
//           if (cap.dequeue(data, len)) {
//               // process data[0..len-1] ...
//               cap.requeue();
//           }
//       }
//   }
//   cap.stop_stream();
//   cap.close();

#pragma once

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <vector>

class V4l2Capture {
public:
    static constexpr unsigned int kNumBuffers = 4;

    V4l2Capture() = default;
    ~V4l2Capture() { close(); }

    V4l2Capture(const V4l2Capture&) = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    // ── Open & configure ──────────────────────────────────────────

    bool open(const char* path, int width, int height, int fps, uint32_t fourcc) {
        close();

        fd_ = ::open(path, O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            fprintf(stderr, "[v4l2] open(%s) failed: %s\n", path, strerror(errno));
            return false;
        }

        v4l2_capability cap{};
        if (xioctl(VIDIOC_QUERYCAP, &cap) < 0) {
            fprintf(stderr, "[v4l2] QUERYCAP failed: %s\n", strerror(errno));
            close(); return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            fprintf(stderr, "[v4l2] %s: not a capture device\n", path);
            close(); return false;
        }
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            fprintf(stderr, "[v4l2] %s: no streaming support\n", path);
            close(); return false;
        }

        // Set format
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<__u32>(width);
        fmt.fmt.pix.height = static_cast<__u32>(height);
        fmt.fmt.pix.pixelformat = fourcc;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "[v4l2] S_FMT failed: %s\n", strerror(errno));
            close(); return false;
        }
        actual_width_  = static_cast<int>(fmt.fmt.pix.width);
        actual_height_ = static_cast<int>(fmt.fmt.pix.height);
        bytesperline_  = static_cast<int>(fmt.fmt.pix.bytesperline);
        if (actual_width_ <= 0 || actual_height_ <= 0) {
            fprintf(stderr, "[v4l2] invalid resolution: %dx%d\n", actual_width_, actual_height_);
            close(); return false;
        }
        fprintf(stderr, "[v4l2] %s: %dx%d stride=%d fourcc=%.4s\n",
                path, actual_width_, actual_height_, bytesperline_,
                reinterpret_cast<const char*>(&fourcc));

        // Set framerate
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<__u32>(fps);
        if (xioctl(VIDIOC_S_PARM, &parm) < 0) {
            fprintf(stderr, "[v4l2] S_PARM %dfps failed (non-fatal)\n", fps);
        }

        // Request mmap buffers
        v4l2_requestbuffers req{};
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        req.count = kNumBuffers;
        if (xioctl(VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "[v4l2] REQBUFS failed: %s\n", strerror(errno));
            close(); return false;
        }
        if (req.count < 2) {
            fprintf(stderr, "[v4l2] too few buffers: %u\n", req.count);
            close(); return false;
        }

        buffers_.resize(req.count);
        for (unsigned int i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (xioctl(VIDIOC_QUERYBUF, &buf) < 0) {
                fprintf(stderr, "[v4l2] QUERYBUF %u failed: %s\n", i, strerror(errno));
                close(); return false;
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                fprintf(stderr, "[v4l2] mmap %u failed: %s\n", i, strerror(errno));
                close(); return false;
            }
        }

        fprintf(stderr, "[v4l2] opened with %zu buffers\n", buffers_.size());
        return true;
    }

    // ── Stream control ────────────────────────────────────────────

    bool start_stream() {
        for (unsigned int i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (xioctl(VIDIOC_QBUF, &buf) < 0) {
                fprintf(stderr, "[v4l2] QBUF %u failed: %s\n", i, strerror(errno));
                return false;
            }
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "[v4l2] STREAMON failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    bool stop_stream() {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMOFF, &type) < 0) {
            fprintf(stderr, "[v4l2] STREAMOFF failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    // ── Frame I/O ─────────────────────────────────────────────────

    // Non-blocking dequeue. Returns true if a frame was ready.
    bool dequeue(const uint8_t*& data, size_t& size) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return false;
            if (dq_errors_++ < 3)
                fprintf(stderr, "[v4l2] DQBUF error: %s\n", strerror(errno));
            return false;
        }

        if (buf.index >= buffers_.size()) {
            fprintf(stderr, "[v4l2] bad buffer index %u\n", buf.index);
            return false;
        }

        dequeued_index_ = buf.index;
        dequeued_size_  = buf.bytesused;
        data = static_cast<const uint8_t*>(buffers_[buf.index].start);
        size = buf.bytesused;
        sequence_   = buf.sequence;
        timestamp_us_ = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000ULL
                      + static_cast<uint64_t>(buf.timestamp.tv_usec);
        return true;
    }

    bool requeue() {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = dequeued_index_;
        if (xioctl(VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[v4l2] QBUF requeue failed: %s\n", strerror(errno));
            return false;
        }
        return true;
    }

    // Block up to timeout_ms for a frame to arrive.
    bool wait_frame(int timeout_ms) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, timeout_ms);
        return ret > 0 && (pfd.revents & POLLIN);
    }

    // ── Accessors ─────────────────────────────────────────────────

    int actual_width()  const { return actual_width_; }
    int actual_height() const { return actual_height_; }
    int bytesperline()  const { return bytesperline_; }
    int fd()            const { return fd_; }

    // ── Cleanup ───────────────────────────────────────────────────

    void close() {
        if (fd_ >= 0) {
            for (auto& b : buffers_) {
                if (b.start) munmap(b.start, b.length);
            }
            buffers_.clear();
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    struct Buffer { void* start = nullptr; size_t length = 0; };

    int xioctl(unsigned long request, void* arg) {
        int r;
        do { r = ::ioctl(fd_, request, arg); } while (r < 0 && errno == EINTR);
        return r;
    }

    int fd_ = -1;
    std::vector<Buffer> buffers_;
    unsigned int dequeued_index_ = 0;
    size_t       dequeued_size_  = 0;
    int actual_width_  = 0;
    int actual_height_ = 0;
    int bytesperline_  = 0;
    uint32_t sequence_ = 0;
    uint64_t timestamp_us_ = 0;
    int dq_errors_ = 0;
};
