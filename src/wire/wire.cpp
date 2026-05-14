#include "wire/wire.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace wlclip::wire {

namespace {

constexpr std::size_t kMaxFdsPerMsg = 28;  // SCM_RIGHTS practical upper bound

std::size_t pad4(std::size_t n) { return (n + 3) & ~std::size_t{3}; }

}  // namespace

// ====== Connection =======================================================

Connection::~Connection() {
    close();
}

void Connection::close() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
    for (int fd : in_fds_)  ::close(fd);
    for (int fd : out_fds_) ::close(fd);
    in_fds_.clear();
    out_fds_.clear();
    in_buf_.clear();
    out_buf_.clear();
}

bool Connection::connect(const std::string& display_name) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        spdlog::error("XDG_RUNTIME_DIR is unset");
        return false;
    }
    std::string name = display_name;
    if (name.empty()) {
        const char* env = std::getenv("WAYLAND_DISPLAY");
        name = env ? env : "wayland-0";
    }

    std::string path;
    if (!name.empty() && name[0] == '/') {
        path = name;
    } else {
        path = std::string(runtime) + "/" + name;
    }

    sock_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        spdlog::error("socket(): {}", std::strerror(errno));
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        spdlog::error("wayland socket path too long: {}", path);
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("connect({}): {}", path, std::strerror(errno));
        ::close(sock_);
        sock_ = -1;
        return false;
    }
    spdlog::debug("wire: connected to {}", path);
    return true;
}

void Connection::out_bytes(const void* data, std::size_t len) {
    auto* p = static_cast<const std::uint8_t*>(data);
    out_buf_.insert(out_buf_.end(), p, p + len);
}

void Connection::out_fd(int fd) {
    out_fds_.push_back(fd);
}

bool Connection::flush() {
    while (!out_buf_.empty() || !out_fds_.empty()) {
        iovec iov{out_buf_.data(), out_buf_.size()};
        // A "no data" send is illegal; we always need at least one byte so
        // that sendmsg actually carries the SCM_RIGHTS ancillary data.
        if (iov.iov_len == 0 && !out_fds_.empty()) {
            // Should not happen — fds are queued alongside a message.
            spdlog::error("wire: attempted to send fds without payload");
            return false;
        }

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        // Build SCM_RIGHTS control message containing up to kMaxFdsPerMsg fds.
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxFdsPerMsg)];
        std::size_t nfd = std::min(out_fds_.size(), kMaxFdsPerMsg);
        if (nfd > 0) {
            msg.msg_control    = ctrl;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
            cmsghdr* cm    = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_RIGHTS;
            cm->cmsg_len   = CMSG_LEN(sizeof(int) * nfd);
            int* fdp = reinterpret_cast<int*>(CMSG_DATA(cm));
            for (std::size_t i = 0; i < nfd; ++i) fdp[i] = out_fds_[i];
        }

        ssize_t n = ::sendmsg(sock_, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("wire: sendmsg(): {}", std::strerror(errno));
            return false;
        }

        out_buf_.erase(out_buf_.begin(), out_buf_.begin() + n);
        // Close the local copies we just handed off — the kernel duplicated
        // them across the socket for the peer.
        for (std::size_t i = 0; i < nfd; ++i) ::close(out_fds_[i]);
        out_fds_.erase(out_fds_.begin(), out_fds_.begin() + nfd);
    }
    return true;
}

bool Connection::wait_readable(int timeout_ms) {
    pollfd pfd{sock_, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) return false;
        spdlog::error("wire: poll(): {}", std::strerror(errno));
        return false;
    }
    return r > 0 && (pfd.revents & POLLIN);
}

ssize_t Connection::pump() {
    std::uint8_t buf[4096];
    iovec iov{buf, sizeof(buf)};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxFdsPerMsg)];

    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control    = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    ssize_t n = ::recvmsg(sock_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 0;
        spdlog::error("wire: recvmsg(): {}", std::strerror(errno));
        return -1;
    }
    if (n == 0) return 0;  // EOF

    in_buf_.insert(in_buf_.end(), buf, buf + n);

    // Collect SCM_RIGHTS fds.
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            std::size_t nfd =
                (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int* fdp = reinterpret_cast<int*>(CMSG_DATA(cm));
            for (std::size_t i = 0; i < nfd; ++i) in_fds_.push_back(fdp[i]);
        }
    }
    if (msg.msg_flags & MSG_CTRUNC) {
        spdlog::warn("wire: control data truncated; some fds were dropped");
    }
    return n;
}

void Connection::in_consume(std::size_t n) {
    if (n > in_buf_.size()) n = in_buf_.size();
    in_buf_.erase(in_buf_.begin(), in_buf_.begin() + n);
}

int Connection::pop_fd() {
    if (in_fds_.empty()) return -1;
    int fd = in_fds_.front();
    in_fds_.pop_front();
    return fd;
}

// ====== Writer ===========================================================

Writer::Writer(std::uint32_t object_id, std::uint16_t opcode) {
    buf_.resize(8);
    std::memcpy(buf_.data(), &object_id, 4);
    // Length field is patched up in finalize().
    std::uint32_t op_len = opcode;
    std::memcpy(buf_.data() + 4, &op_len, 4);
}

void Writer::u32(std::uint32_t v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    buf_.insert(buf_.end(), b, b + 4);
}

void Writer::i32(std::int32_t v) {
    u32(static_cast<std::uint32_t>(v));
}

void Writer::str(std::string_view s) {
    std::uint32_t len = static_cast<std::uint32_t>(s.size() + 1);  // incl NUL
    u32(len);
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
    while (buf_.size() % 4 != 0) buf_.push_back(0);
}

std::vector<std::uint8_t> Writer::finalize() {
    std::uint32_t total = static_cast<std::uint32_t>(buf_.size());
    // High 16 bits of opcode_and_length is the length in bytes.
    std::uint32_t op_len;
    std::memcpy(&op_len, buf_.data() + 4, 4);
    op_len = (op_len & 0xFFFFu) | (total << 16);
    std::memcpy(buf_.data() + 4, &op_len, 4);
    return std::move(buf_);
}

// ====== Reader ===========================================================

Reader::Reader(std::uint32_t object_id, std::uint16_t opcode,
               const std::uint8_t* payload, std::size_t payload_len)
    : object_id_(object_id), opcode_(opcode),
      data_(payload), len_(payload_len) {}

std::uint32_t Reader::u32() {
    if (cur_ + 4 > len_) {
        spdlog::error("wire: reader underrun (need 4, have {})", len_ - cur_);
        return 0;
    }
    std::uint32_t v;
    std::memcpy(&v, data_ + cur_, 4);
    cur_ += 4;
    return v;
}

std::int32_t Reader::i32() {
    return static_cast<std::int32_t>(u32());
}

std::string Reader::str() {
    std::uint32_t len = u32();
    if (len == 0 || cur_ + pad4(len) > len_) {
        spdlog::error("wire: bad string length {}", len);
        return {};
    }
    // `len` includes the trailing NUL.
    std::string s(reinterpret_cast<const char*>(data_ + cur_), len - 1);
    cur_ += pad4(len);
    return s;
}

std::size_t peek_message_size(const std::uint8_t* buf, std::size_t avail) {
    if (avail < 8) return 0;
    std::uint32_t op_len;
    std::memcpy(&op_len, buf + 4, 4);
    std::size_t total = (op_len >> 16) & 0xFFFFu;
    if (total < 8) return 0;          // malformed; caller should error out
    if (avail < total) return 0;
    return total;
}

}  // namespace wlclip::wire
