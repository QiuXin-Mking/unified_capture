#include "app/socket_server.h"

#include <cassert>

int main() {
    assert(parse_socket_command("start\n").kind == SocketCommandKind::start);
    assert(parse_socket_command("stop").kind == SocketCommandKind::stop);

    SocketCommand preview = parse_socket_command("preview:/tmp/p.jpg\n");
    assert(preview.kind == SocketCommandKind::preview);
    assert(preview.preview_path == "/tmp/p.jpg");

    assert(parse_socket_command("status").kind == SocketCommandKind::status);
    assert(parse_socket_command("unknown").kind == SocketCommandKind::unknown);
    assert(parse_socket_command("preview:").kind == SocketCommandKind::unknown);
    assert(parse_socket_command("start\n\n").kind == SocketCommandKind::unknown);
}
