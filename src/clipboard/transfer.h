#pragma once

#include <cstddef>
#include <cstdint>

namespace wlclip::clipboard::detail {

// Write `len` bytes from `data` to `fd` (which is consumed/closed), tolerating
// EINTR/EAGAIN with a short timeout. Used by source `send` callbacks in both
// backends.
//
// `fd` is taken by value: the caller relinquishes ownership; this function
// closes it before returning.
void serve_send(int fd, const void* data, std::size_t len);

}
