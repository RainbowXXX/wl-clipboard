#pragma once

#include "clipboard/backend.h"

#include <memory>
#include <string_view>

namespace wlclip::wayland { class State; }

namespace wlclip::clipboard {

enum class BackendKind {
    Auto,
    DataControl,    // zwlr_data_control_v1
    DataDevice,     // wl_data_device_manager
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
std::unique_ptr<Backend> make_backend(wayland::State& state, BackendKind kind);

}
