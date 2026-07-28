#pragma once

#include <functional>
#include <string>
#include <string_view>

enum class SocketCommandKind { start, stop, preview, status, unknown };

struct SocketCommand {
    SocketCommandKind kind;
    std::string preview_path;
};

SocketCommand parse_socket_command(std::string_view request);

class SocketServer {
public:
    explicit SocketServer(std::string path = "/tmp/unified_capture.sock");
    ~SocketServer();

    bool open();
    int fd() const;
    void close();
    void serve_one(
        const std::function<std::string(const SocketCommand&)>& handler);

private:
    std::string path_;
    int fd_ = -1;
};
