// Minimal hand-rolled Wayland wire protocol — just enough to drive
// wl_display + wl_registry + wl_seat + zwlr_data_control_unstable_v1.
//
// All multi-byte fields are little-endian on the wire (Wayland spec assumes
// the host byte order matches the compositor; in practice all current
// Wayland implementations are little-endian).
//
// Message framing:
//   [u32  object_id][u32 opcode_and_length]
//   opcode  = opcode_and_length & 0xFFFF
//   length  = (opcode_and_length >> 16)   (total bytes incl. header,
//                                           always a multiple of 4)
//
// Argument types (all 4-byte aligned, padded with zeros):
//   int, uint, fixed, object, new_id : 4 bytes
//   string  : u32 length (incl. trailing NUL) + bytes + pad
//   array   : u32 length + bytes + pad
//   fd      : transmitted out-of-band via SCM_RIGHTS

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wlclip::wire {

// --- Wire protocol object IDs --------------------------------------------

constexpr std::uint32_t kDisplayId = 1;
constexpr std::uint32_t kClientIdMin = 2;
constexpr std::uint32_t kServerIdMin = 0xFF000000u;

// --- A buffered Wayland socket connection with SCM_RIGHTS fd passing -----

class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Connect to $WAYLAND_DISPLAY (or given name) under $XDG_RUNTIME_DIR.
    // Returns false on failure (logs the reason).
    bool connect(const std::string& display_name = "");

    int  fd() const { return sock_; }
    bool ok() const { return sock_ >= 0; }
    void close();

    // ---- Sending ----
    // Append raw bytes to the outgoing buffer.
    void out_bytes(const void* data, std::size_t len);
    // Queue an fd to be sent with the next flush.
    void out_fd(int fd);
    // Send everything queued. Returns false on permanent error.
    bool flush();

    // ---- Receiving ----
    // Block (with timeout in ms; -1 = forever) until at least one byte is
    // readable or fd is closed. Returns false on error/timeout.
    bool wait_readable(int timeout_ms);
    // Read more bytes/fds into the internal queue (non-blocking).
    // Returns -1 on error, 0 on EOF, otherwise bytes appended.
    ssize_t pump();

    // Peek/consume incoming bytes.
    std::size_t in_size() const { return in_buf_.size(); }
    const std::uint8_t* in_data() const { return in_buf_.data(); }
    void in_consume(std::size_t n);

    // Pop the next file descriptor delivered by the compositor. Returns -1
    // if none queued.
    int  pop_fd();

private:
    int sock_ = -1;
    std::vector<std::uint8_t> in_buf_;
    std::deque<int>           in_fds_;
    std::vector<std::uint8_t> out_buf_;
    std::deque<int>           out_fds_;
};

// --- Encoders ------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::uint32_t object_id, std::uint16_t opcode);

    void u32(std::uint32_t v);
    void i32(std::int32_t v);
    void str(std::string_view s);  // trailing NUL added, padded to u32

    // Finalize the message header (writes total length into bytes 4..7)
    // and return the assembled bytes.
    std::vector<std::uint8_t> finalize();

private:
    std::vector<std::uint8_t> buf_;
};

// --- Decoders ------------------------------------------------------------

// Wraps a single received message (header already parsed) and provides
// typed reads from the payload.
class Reader {
public:
    Reader(std::uint32_t object_id, std::uint16_t opcode,
           const std::uint8_t* payload, std::size_t payload_len);

    std::uint32_t object_id() const { return object_id_; }
    std::uint16_t opcode()    const { return opcode_; }

    std::uint32_t u32();
    std::int32_t  i32();
    std::string   str();

    bool eof() const { return cur_ >= len_; }

private:
    std::uint32_t object_id_;
    std::uint16_t opcode_;
    const std::uint8_t* data_;
    std::size_t cur_ = 0;
    std::size_t len_;
};

// Try to parse one complete message out of `buf`. Returns the total wire
// length consumed (0 if buffer doesn't yet contain a full message).
std::size_t peek_message_size(const std::uint8_t* buf, std::size_t avail);

}  // namespace wlclip::wire
