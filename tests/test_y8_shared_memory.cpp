#include "hardware/video/y8_shared_memory.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

std::string unique_name(const char* prefix) {
    return std::string(prefix) + std::to_string(static_cast<long long>(getpid()));
}

int connect_client(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0);
    return fd;
}

std::string read_notifications(int fd) {
    std::string result;
    char buffer[512];
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    while (result.find("FRAME ") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) {
            result.append(buffer, static_cast<size_t>(count));
        } else {
            usleep(1000);
        }
    }
    return result;
}

}  // namespace

int main() {
    const std::string socket_path = unique_name("/tmp/y8-shm-test-");
    const std::string shm_name = unique_name("/y8-shm-test-");
    Y8SharedMemoryPublisher publisher;
    assert(publisher.open(socket_path, shm_name, 4, 2, 8));

    const int client = connect_client(socket_path);
    const uint8_t frame[] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(publisher.publish(frame, sizeof(frame), 12, 3456));

    const std::string notifications = read_notifications(client);
    assert(notifications.find("Y8_SHM 1 ") != std::string::npos);
    assert(notifications.find("width=4 height=2 bytes=8 slots=8") !=
           std::string::npos);
    assert(notifications.find("FRAME 12 slot=4 pts_us=3456 bytes=8") !=
           std::string::npos);

    const Y8SharedMemoryView view = publisher.latest_view();
    assert(view.sequence == 12);
    assert(view.pts_us == 3456);
    assert(view.width == 4);
    assert(view.height == 2);
    assert(view.bytes == sizeof(frame));
    assert(std::memcmp(view.data, frame, sizeof(frame)) == 0);

    close(client);
    publisher.close();
}
