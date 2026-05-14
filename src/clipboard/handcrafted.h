// Backend that speaks zwlr_data_control_unstable_v1 directly over a raw
// AF_UNIX socket — no libwayland-client. Useful for verifying byte-for-byte
// behaviour against the compositor (or for systems where libwayland's
// internal state has been observed to interact badly with the compositor).
//
// Only the subset of the Wayland wire protocol needed for clipboard
// management is implemented:
//   wl_display, wl_registry, wl_callback, wl_seat,
//   zwlr_data_control_{manager,device,source,offer}_v1.

#pragma once

#include "clipboard/backend.h"

#include <cstdint>
#include <string>

namespace wlclip::wayland { struct SeatInfo; }

namespace wlclip::clipboard {

class HandcraftedBackend final : public Backend {
public:
    // The handcrafted backend ignores the libwayland SeatInfo and instead
    // resolves a seat via its own connection. Pass the user-visible seat
    // selector (index or wl_seat.name) directly.
    HandcraftedBackend(std::string display, std::string seat)
        : display_(std::move(display)), seat_(std::move(seat)) {}

    const char* name() const override {
        return "zwlr_data_control_v1 (handcrafted)";
    }

    bool copy(const wayland::SeatInfo& /*ignored*/, Selection sel,
              CopyData data, bool oneshot) override;
    bool paste(const wayland::SeatInfo& /*ignored*/, Selection sel,
               const std::string& prefer_mime, bool list_only,
               PasteResult& out) override;

private:
    std::string display_;
    std::string seat_;
};

}  // namespace wlclip::clipboard
