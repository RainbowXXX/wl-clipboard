#include "clipboard/factory.h"

#include "clipboard/data_control.h"
#include "clipboard/data_device.h"
#include "wayland/state.h"

namespace wlclip::clipboard {

std::unique_ptr<Backend> make_backend(wayland::State& state,
                                      std::string_view which) {
    if (which == "wlr") {
        if (!state.data_control_manager()) return nullptr;
        return std::make_unique<DataControlBackend>(state);
    }
    if (which == "wl") {
        if (!state.data_device_manager()) return nullptr;
        return std::make_unique<DataDeviceBackend>(state);
    }
    // auto / empty: prefer wlr_data_control.
    if (state.data_control_manager()) return std::make_unique<DataControlBackend>(state);
    if (state.data_device_manager())  return std::make_unique<DataDeviceBackend>(state);
    return nullptr;
}

}
