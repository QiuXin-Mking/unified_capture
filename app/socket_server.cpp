#include "app/socket_server.h"

#include "core/product_config.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

bool is_preview_channel(std::string_view channel) {
    return channel == "wrist_left" || channel == "wrist_right" ||
           channel == "jhh04" || channel == "jhh02" || channel == "head";
}

bool is_absolute_path(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

}  // namespace

SocketCommand parse_socket_command(std::string_view request) {
    if (!request.empty() && request.back() == '\n') {
        request.remove_suffix(1);
    }
    if (request == "start") {
        return {SocketCommandKind::start, {}, {}};
    }
    if (request == "stop") {
        return {SocketCommandKind::stop, {}, {}};
    }
    if (request == "status") {
        return {SocketCommandKind::status, {}, {}};
    }

    constexpr std::string_view set_product_prefix = "set_product:";
    if (request.starts_with(set_product_prefix) &&
        request.size() > set_product_prefix.size()) {
        const std::string_view product = request.substr(set_product_prefix.size());
        if (parse_product_profile(product).has_value()) {
            return {SocketCommandKind::set_product, {}, {}, std::string(product)};
        }
        return {SocketCommandKind::unknown, {}, {}};
    }

    constexpr std::string_view prefix = "preview:";
    if (request.starts_with(prefix) && request.size() > prefix.size()) {
        const std::string_view suffix = request.substr(prefix.size());
        const size_t separator = suffix.find(':');
        if (separator != std::string_view::npos &&
            is_preview_channel(suffix.substr(0, separator))) {
            const std::string_view path = suffix.substr(separator + 1);
            if (is_absolute_path(path)) {
                return {SocketCommandKind::preview,
                        std::string(suffix.substr(0, separator)),
                        std::string(path)};
            }
            return {SocketCommandKind::unknown, {}, {}};
        }
        if (is_absolute_path(suffix)) {
            return {SocketCommandKind::preview, {}, std::string(suffix)};
        }
    }
    return {SocketCommandKind::unknown, {}, {}};
}

SocketServer::SocketServer(std::string path)
    : path_(std::move(path)) {}

SocketServer::~SocketServer() {
    close();
}

bool SocketServer::open() {
    if (fd_ >= 0) {
        return true;
    }

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return false;
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1);

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) < 0) {
            unlink(path_.c_str());
        }
        ::close(probe);
    }

    if (bind(server, reinterpret_cast<struct sockaddr*>(&address),
             sizeof(address)) < 0) {
        perror("bind");
        ::close(server);
        return false;
    }
    if (listen(server, 4) < 0) {
        perror("listen");
        ::close(server);
        unlink(path_.c_str());
        return false;
    }

    int flags = fcntl(server, F_GETFL);
    fcntl(server, F_SETFL, flags | O_NONBLOCK);
    fd_ = server;
    printf("[socket] listening on %s (poll mode)\n", path_.c_str());
    return true;
}

int SocketServer::fd() const {
    return fd_;
}

void SocketServer::close() {
    if (fd_ < 0) {
        return;
    }
    ::close(fd_);
    fd_ = -1;
    unlink(path_.c_str());
}

void SocketServer::serve_one(
    const std::function<std::string(const SocketCommand&)>& handler) {
    if (fd_ < 0) {
        return;
    }

    int client = accept(fd_, nullptr, nullptr);
    if (client < 0) {
        return;
    }

    char buffer[256];
    ssize_t size = read(client, buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        ::close(client);
        return;
    }

    std::string response =
        handler(parse_socket_command(std::string_view(buffer, size)));
    response += '\n';
    const char* data = response.data();
    size_t remaining = response.size();
    while (remaining > 0) {
        ssize_t written = write(client, data, remaining);
        if (written <= 0) {
            break;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    ::close(client);
}
