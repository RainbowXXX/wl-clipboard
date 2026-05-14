#include "clipboard/transfer.h"

#include "core/fd.h"
#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace wlclip::clipboard::detail {

void serve_send(int raw_fd, const void* data, std::size_t len) {
    core::Fd owned(raw_fd);
    int flags = ::fcntl(owned.get(), F_GETFL, 0);
    if (flags >= 0) ::fcntl(owned.get(), F_SETFL, flags | O_NONBLOCK);

    const auto* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::write(owned.get(), p, len);
        if (n > 0) {
            p   += n;
            len -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{owned.get(), POLLOUT, 0};
            if (::poll(&pfd, 1, 5000) <= 0) {
                spdlog::warn("transfer: write timed out, dropping {} bytes", len);
                break;
            }
            continue;
        }
        if (n < 0) {
            spdlog::warn("transfer: write failed: {}", std::strerror(errno));
            break;
        }
        break;  // n == 0
    }
}

}
