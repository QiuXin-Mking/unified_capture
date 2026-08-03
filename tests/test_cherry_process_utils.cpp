#include "hardware/cherry/cherry_process_utils.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
    char executable_dir[] = "/tmp/cherry-executable-XXXXXX";
    assert(mkdtemp(executable_dir) != nullptr);
    const std::string executable_path =
        std::string(executable_dir) + "/fake-ffmpeg";
    const int executable_fd = open(
        executable_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    assert(executable_fd >= 0);
    assert(write(executable_fd, "#!/bin/sh\n", 10) == 10);
    assert(close(executable_fd) == 0);
    assert(chmod(executable_path.c_str(), 0755) == 0);
    const std::string search_path =
        "/definitely/missing:" + std::string(executable_dir);
    assert(resolve_cherry_executable("fake-ffmpeg", search_path) ==
           executable_path);
    assert(resolve_cherry_executable(executable_path, search_path) ==
           executable_path);
    assert(resolve_cherry_executable("missing-ffmpeg", search_path).empty());
    assert(unlink(executable_path.c_str()) == 0);
    assert(rmdir(executable_dir) == 0);

    char path[] = "/tmp/cherry-cloexec-XXXXXX";
    const int seed_fd = mkstemp(path);
    assert(seed_fd >= 0);
    assert(close(seed_fd) == 0);

    std::string error;
    FILE* output = open_cherry_cloexec_output(path, error);
    assert(output != nullptr);
    assert(error.empty());
    const int output_fd = fileno(output);
    assert(output_fd > 2);
    const int descriptor_flags = fcntl(output_fd, F_GETFD);
    assert(descriptor_flags >= 0);
    assert((descriptor_flags & FD_CLOEXEC) != 0);
    assert(fclose(output) == 0);
    assert(unlink(path) == 0);

    int inherited_pipe[2] = {-1, -1};
    assert(pipe(inherited_pipe) == 0);
    const int fallback_limit = cherry_inherited_fd_limit();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close_cherry_inherited_fds(3, fallback_limit);
        const bool read_closed =
            fcntl(inherited_pipe[0], F_GETFD) < 0 && errno == EBADF;
        const bool write_closed =
            fcntl(inherited_pipe[1], F_GETFD) < 0 && errno == EBADF;
        _exit(read_closed && write_closed ? 0 : 1);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(close(inherited_pipe[0]) == 0);
    assert(close(inherited_pipe[1]) == 0);
    return 0;
}
