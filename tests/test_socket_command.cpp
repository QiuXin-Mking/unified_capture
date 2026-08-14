#include "app/socket_server.h"

#include <cassert>

int main() {
    assert(parse_socket_command("start\n").kind == SocketCommandKind::start);
    assert(parse_socket_command("stop").kind == SocketCommandKind::stop);

    SocketCommand preview = parse_socket_command("preview:/tmp/p.jpg\n");
    assert(preview.kind == SocketCommandKind::preview);
    assert(preview.preview_channel.empty());
    assert(preview.preview_path == "/tmp/p.jpg");

    SocketCommand addressed = parse_socket_command(
        "preview:wrist_left:/tmp/wrist-left.jpg\n");
    assert(addressed.kind == SocketCommandKind::preview);
    assert(addressed.preview_channel == "wrist_left");
    assert(addressed.preview_path == "/tmp/wrist-left.jpg");

    for (const char* channel : {"wrist_left", "wrist_right", "jhh04", "jhh02"}) {
        SocketCommand channel_preview = parse_socket_command(
            std::string("preview:") + channel + ":/tmp/p.jpg");
        assert(channel_preview.kind == SocketCommandKind::preview);
        assert(channel_preview.preview_channel == channel);
        assert(channel_preview.preview_path == "/tmp/p.jpg");
    }

    assert(parse_socket_command("status").kind == SocketCommandKind::status);

    SocketCommand set_product = parse_socket_command("set_product:banana\n");
    assert(set_product.kind == SocketCommandKind::set_product);
    assert(set_product.product == "banana");
    assert(parse_socket_command("set_product:mango").product == "mango");
    assert(parse_socket_command("set_product:cherry").product == "cherry");
    assert(parse_socket_command("set_product:kiwi").kind == SocketCommandKind::unknown);
    assert(parse_socket_command("set_product:").kind == SocketCommandKind::unknown);

    assert(parse_socket_command("unknown").kind == SocketCommandKind::unknown);
    assert(parse_socket_command("preview:").kind == SocketCommandKind::unknown);
    assert(parse_socket_command("preview:unknown:/tmp/x.jpg").kind ==
           SocketCommandKind::unknown);
    assert(parse_socket_command("preview:jhh02:relative.jpg").kind ==
           SocketCommandKind::unknown);
    assert(parse_socket_command("preview:relative.jpg").kind ==
           SocketCommandKind::unknown);
    assert(parse_socket_command("start\n\n").kind == SocketCommandKind::unknown);
}
