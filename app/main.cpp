#include "app/runtime.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

Runtime* g_runtime = nullptr;

void signal_handler(int) {
    if (g_runtime) {
        g_runtime->keep_running() = false;
        g_runtime->session_running() = false;
    }
}

void print_usage(const char* program) {
    printf("Usage: %s [OPTIONS] [output_prefix]\n", program);
}

}  // namespace

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    setlinebuf(stdout);
    setlinebuf(stderr);

    RuntimeOptions options;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scan")) {
            options.scan_only = true;
            break;
        }
        if (!strcmp(argv[i], "--no-gpio")) {
            options.use_gpio = false;
        } else if (!strcmp(argv[i], "--socket")) {
            options.socket_mode = true;
            options.use_gpio = false;
        } else if (!strcmp(argv[i], "--no-as5600")) {
            options.use_as5600 = false;
        } else if (!strcmp(argv[i], "--no-imu")) {
            options.use_imu = false;
        } else if (!strcmp(argv[i], "--no-h265")) {
            options.use_h265 = false;
        } else if (!strcmp(argv[i], "--single")) {
            options.single_shot = true;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            options.output_prefix = argv[i];
        }
    }

    Runtime runtime(std::move(options));
    g_runtime = &runtime;
    return runtime.run();
}
