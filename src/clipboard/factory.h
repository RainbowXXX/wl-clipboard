#pragma once

#include "clipboard/backend.h"

#include <memory>
#include <string_view>

namespace wlclip::wayland { class State; }

namespace wlclip::clipboard {

enum class BackendKind {
    Auto,
    DataControl,        // zwlr_data_control_v1 (via libwayland)
    DataDevice,         // wl_data_device_manager (via libwayland)
    HandcraftedControl, // zwlr_data_control_v1 hand-rolled (no libwayland)
    X11,                // X11 CLIPBOARD/PRIMARY via libxcb
};

// Parse a user-supplied backend/protocol selector. Accepts (case sensitive):
//   "" / "auto"
//   "wlr" / "data-control" / "zwlr_data_control_v1" / "zwlr_data_control_unstable_v1"
//   "wl"  / "data-device"  / "wl_data_device_manager"
// Returns false if `name` is not recognised.
bool parse_backend_kind(std::string_view name, BackendKind& out);

// One-line human-readable list of accepted selector names (for error messages).
const char* backend_kind_names();

// Construct a backend instance. Returns nullptr if the requested protocol is
// not advertised by the compositor (or, for Auto, if neither is).
//
// The libwayland-using backends (DataControl, DataDevice) consult `state`
// to find globals and seats. The handcrafted backend opens its own raw
// connection; `display` and `seat` come from the CLI options.
std::unique_ptr<Backend> make_backend(wayland::State& state, BackendKind kind,
                                      const std::string& display,
                                      const std::string& seat);

}
