#pragma once

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

inline FILE* open_cherry_cloexec_output(const std::string& path,
                                        std::string& error) {
    error.clear();
    const int fd = open(path.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "cannot create " + path + ": " + strerror(errno);
        return nullptr;
    }
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        const int saved_errno = errno;
        close(fd);
        error = "cannot protect " + path + " from exec: " +
                strerror(saved_errno);
        return nullptr;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        const int saved_errno = errno;
        close(fd);
        error = "cannot open stream " + path + ": " +
                strerror(saved_errno);
    }
    return file;
}

inline int cherry_inherited_fd_limit() {
    const long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0) return 65536;
    return limit > INT_MAX ? INT_MAX : static_cast<int>(limit);
}

// Called in the post-fork child. All values are computed by the parent, and
// both the syscall and fallback use only async-signal-safe operations.
inline void close_cherry_inherited_fds(unsigned int first_fd,
                                       int fallback_limit) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, first_fd, ~0U, 0) == 0) return;
#endif
    for (int fd = static_cast<int>(first_fd); fd < fallback_limit; ++fd) {
        close(fd);
    }
}
