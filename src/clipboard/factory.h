#pragma once

#include "clipboard/backend.h"

#include <memory>
#include <string_view>

namespace wlclip::wayland { class State; }

namespace wlclip::clipboard {

// Pick a backend by name ("wlr", "wl", "auto" or empty). Returns nullptr if
// the requested backend is not advertised by the compositor.
std::unique_ptr<Backend> make_backend(wayland::State& state,
                                      std::string_view which);

}
