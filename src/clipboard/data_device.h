#pragma once

#include "clipboard/backend.h"

namespace wlclip::wayland { class State; }

namespace wlclip::clipboard {

// Backend over the core wl_data_device_manager protocol.
//
// Intentionally does NOT create a surface and grab focus. Most compositors
// require keyboard focus to deliver selection events or accept set_selection,
// so this backend will log a warning and fail rather than hang. It exists
// for parity with the protocol set and for environments where the focus
// requirement is relaxed.
class DataDeviceBackend final : public Backend {
public:
    explicit DataDeviceBackend(wayland::State& state) : state_(state) {}

    const char* name() const override { return "wl_data_device_manager"; }

    bool copy(const wayland::SeatInfo& seat, Selection sel,
              CopyData data, bool oneshot) override;
    bool paste(const wayland::SeatInfo& seat, Selection sel,
               const std::string& prefer_mime, bool list_only,
               PasteResult& out) override;

private:
    wayland::State& state_;
};

}
