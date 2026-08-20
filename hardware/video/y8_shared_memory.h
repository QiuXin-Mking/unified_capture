#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

constexpr uint32_t kY8SharedMemoryMagic = 0x59385348;  // "Y8SH"
constexpr uint32_t kY8SharedMemoryVersion = 1;
constexpr uint32_t kY8SharedMemorySlots = 8;

struct Y8SharedMemoryHeader {
    uint32_t magic = kY8SharedMemoryMagic;
    uint32_t version = kY8SharedMemoryVersion;
    uint32_t slot_count = kY8SharedMemorySlots;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_bytes = 0;
    uint64_t reserved[2] = {};
};

struct alignas(8) Y8SharedMemorySlotHeader {
    std::atomic<uint64_t> sequence{0};
    uint64_t pts_us = 0;
    uint32_t bytes = 0;
    uint32_t reserved = 0;
};

struct Y8SharedMemoryView {
    const uint8_t* data = nullptr;
    uint64_t sequence = 0;
    uint64_t pts_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes = 0;
};

inline size_t y8_shared_memory_slot_offset(size_t slot,
                                           size_t frame_bytes) {
    return sizeof(Y8SharedMemoryHeader) +
           slot * (sizeof(Y8SharedMemorySlotHeader) + frame_bytes);
}

class Y8SharedMemoryPublisher {
public:
    Y8SharedMemoryPublisher() = default;
    ~Y8SharedMemoryPublisher() { close(); }

    Y8SharedMemoryPublisher(const Y8SharedMemoryPublisher&) = delete;
    Y8SharedMemoryPublisher& operator=(const Y8SharedMemoryPublisher&) = delete;

    bool open(const std::string& socket_path,
              const std::string& shm_name,
              uint32_t width,
              uint32_t height,
              uint32_t slots = kY8SharedMemorySlots) {
        close();
        if (socket_path.empty() || shm_name.empty() || shm_name.front() != '/' ||
            width == 0 || height == 0 || slots == 0 ||
            width > UINT32_MAX / height) {
            return false;
        }
        if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
            return false;
        }

        width_ = width;
        height_ = height;
        frame_bytes_ = width * height;
        slots_ = slots;
        shm_name_ = shm_name;
        socket_path_ = socket_path;
        mapping_bytes_ = sizeof(Y8SharedMemoryHeader) +
                         slots_ * (sizeof(Y8SharedMemorySlotHeader) + frame_bytes_);

        shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
        if (shm_fd_ >= 0) {
            shm_created_ = true;
        }
        if (shm_fd_ < 0 || ftruncate(shm_fd_, static_cast<off_t>(mapping_bytes_)) != 0) {
            cleanup_shm();
            return false;
        }
        mapping_ = mmap(nullptr, mapping_bytes_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd_, 0);
        if (mapping_ == MAP_FAILED) {
            mapping_ = nullptr;
            cleanup_shm();
            return false;
        }
        std::memset(mapping_, 0, mapping_bytes_);
        auto* header = static_cast<Y8SharedMemoryHeader*>(mapping_);
        header->magic = kY8SharedMemoryMagic;
        header->version = kY8SharedMemoryVersion;
        header->slot_count = slots_;
        header->width = width_;
        header->height = height_;
        header->frame_bytes = frame_bytes_;

        socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd_ < 0 || !set_nonblocking(socket_fd_)) {
            close();
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(),
                    socket_path_.size() + 1);
        if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != 0) {
            close();
            return false;
        }
        socket_bound_ = true;
        if (listen(socket_fd_, 4) != 0) {
            close();
            return false;
        }
        chmod(socket_path_.c_str(), 0660);
        opened_ = true;
        return true;
    }

    bool publish(const uint8_t* data,
                 size_t bytes,
                 uint64_t frame_idx,
                 uint64_t pts_us) {
        if (!opened_ || !data || bytes != frame_bytes_ || frame_idx == UINT64_MAX) {
            return false;
        }
        accept_clients();
        const uint64_t sequence = frame_idx + 1;
        const size_t slot = static_cast<size_t>(frame_idx % slots_);
        auto* slot_header = slot_header_at(slot);
        uint8_t* slot_data = slot_data_at(slot);
        slot_header->pts_us = pts_us;
        slot_header->bytes = static_cast<uint32_t>(bytes);
        std::memcpy(slot_data, data, bytes);
        slot_header->sequence.store(sequence, std::memory_order_release);
        latest_sequence_ = sequence;
        notify_clients(sequence, slot, pts_us);
        return true;
    }

    Y8SharedMemoryView latest_view() const {
        Y8SharedMemoryView view;
        if (!opened_ || latest_sequence_ == 0) {
            return view;
        }
        const size_t slot = static_cast<size_t>((latest_sequence_ - 1) % slots_);
        const auto* slot_header = slot_header_at(slot);
        if (slot_header->sequence.load(std::memory_order_acquire) !=
            latest_sequence_) {
            return view;
        }
        view.data = slot_data_at(slot);
        view.sequence = latest_sequence_ - 1;
        view.pts_us = slot_header->pts_us;
        view.width = width_;
        view.height = height_;
        view.bytes = slot_header->bytes;
        return view;
    }

    bool opened() const { return opened_; }
    const std::string& socket_path() const { return socket_path_; }
    const std::string& shm_name() const { return shm_name_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t frame_bytes() const { return frame_bytes_; }
    uint32_t slots() const { return slots_; }

    void close() {
        for (const int client : clients_) {
            ::close(client);
        }
        clients_.clear();
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        if (socket_bound_ && !socket_path_.empty()) {
            unlink(socket_path_.c_str());
        }
        socket_bound_ = false;
        if (mapping_) {
            munmap(mapping_, mapping_bytes_);
            mapping_ = nullptr;
        }
        cleanup_shm();
        opened_ = false;
        latest_sequence_ = 0;
    }

private:
    Y8SharedMemorySlotHeader* slot_header_at(size_t slot) {
        return reinterpret_cast<Y8SharedMemorySlotHeader*>(
            static_cast<uint8_t*>(mapping_) +
            y8_shared_memory_slot_offset(slot, frame_bytes_));
    }

    const Y8SharedMemorySlotHeader* slot_header_at(size_t slot) const {
        return reinterpret_cast<const Y8SharedMemorySlotHeader*>(
            static_cast<const uint8_t*>(mapping_) +
            y8_shared_memory_slot_offset(slot, frame_bytes_));
    }

    uint8_t* slot_data_at(size_t slot) {
        return reinterpret_cast<uint8_t*>(slot_header_at(slot)) +
               sizeof(Y8SharedMemorySlotHeader);
    }

    const uint8_t* slot_data_at(size_t slot) const {
        return reinterpret_cast<const uint8_t*>(slot_header_at(slot)) +
               sizeof(Y8SharedMemorySlotHeader);
    }

    void accept_clients() {
        if (socket_fd_ < 0) {
            return;
        }
        while (true) {
            const int client = accept(socket_fd_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                return;
            }
            if (!set_nonblocking(client)) {
                ::close(client);
                continue;
            }
            const std::string handshake =
                "Y8_SHM 1 " + shm_name_ +
                " width=" + std::to_string(width_) +
                " height=" + std::to_string(height_) +
                " bytes=" + std::to_string(frame_bytes_) +
                " slots=" + std::to_string(slots_) + "\n";
            if (send_message(client, handshake)) {
                clients_.push_back(client);
            } else {
                ::close(client);
            }
        }
    }

    void notify_clients(uint64_t sequence, size_t slot, uint64_t pts_us) {
        const std::string message =
            "FRAME " + std::to_string(sequence - 1) +
            " slot=" + std::to_string(slot) +
            " pts_us=" + std::to_string(pts_us) +
            " bytes=" + std::to_string(frame_bytes_) + "\n";
        for (size_t i = 0; i < clients_.size();) {
            if (!send_message(clients_[i], message)) {
                ::close(clients_[i]);
                clients_.erase(clients_.begin() + static_cast<ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    static bool send_message(int fd, const std::string& message) {
        const int flags =
#ifdef MSG_NOSIGNAL
            MSG_DONTWAIT | MSG_NOSIGNAL;
#else
            MSG_DONTWAIT;
#endif
        const ssize_t sent = send(fd, message.data(), message.size(), flags);
        return sent == static_cast<ssize_t>(message.size());
    }

    static bool set_nonblocking(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void cleanup_shm() {
        if (shm_fd_ >= 0) {
            ::close(shm_fd_);
            shm_fd_ = -1;
        }
        if (shm_created_ && !shm_name_.empty()) {
            shm_unlink(shm_name_.c_str());
        }
        shm_created_ = false;
    }

    bool opened_ = false;
    bool shm_created_ = false;
    bool socket_bound_ = false;
    int shm_fd_ = -1;
    int socket_fd_ = -1;
    void* mapping_ = nullptr;
    size_t mapping_bytes_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frame_bytes_ = 0;
    uint32_t slots_ = 0;
    uint64_t latest_sequence_ = 0;
    std::string socket_path_;
    std::string shm_name_;
    std::vector<int> clients_;
};
