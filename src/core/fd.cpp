#include "core/fd.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace wlclip::core {

void Fd::reset(int fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

Pipe make_pipe() {
    int fds[2];
    if (::pipe(fds) < 0) {
        spdlog::error("pipe(): {}", std::strerror(errno));
        return {};
    }
    return {Fd(fds[0]), Fd(fds[1])};
}

std::vector<std::byte> drain_fd(int fd) {
    std::vector<std::byte> buf;
    constexpr std::size_t chunk = 64 * 1024;
    while (true) {
        std::size_t old = buf.size();
        buf.resize(old + chunk);
        ssize_t n = ::read(fd, buf.data() + old, chunk);
        if (n > 0) {
            buf.resize(old + static_cast<std::size_t>(n));
        } else if (n == 0) {
            buf.resize(old);
            break;
        } else {
            buf.resize(old);
            if (errno == EINTR) continue;
            spdlog::error("read(): {}", std::strerror(errno));
            break;
        }
    }
    return buf;
}

bool write_all(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("write(): {}", std::strerror(errno));
            return false;
        }
        p += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

}
