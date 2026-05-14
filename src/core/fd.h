#pragma once

#include <cstddef>
#include <vector>

namespace wlclip::core {

// RAII wrapper for a POSIX file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~Fd() { reset(); }

    int  get() const     { return fd_; }
    int  release()       { int f = fd_; fd_ = -1; return f; }
    void reset(int fd = -1);
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

struct Pipe {
    Fd read_end;
    Fd write_end;
};

Pipe make_pipe();

// Read every byte from fd until EOF.
std::vector<std::byte> drain_fd(int fd);

// write(2) all bytes, handling EINTR / partial writes.
bool write_all(int fd, const void* data, std::size_t len);

}
