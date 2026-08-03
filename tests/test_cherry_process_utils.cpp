#include "hardware/cherry/cherry_process_utils.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
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
