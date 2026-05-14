#include "clipboard/factory.h"

#include "clipboard/data_control.h"
#include "clipboard/data_device.h"
#include "core/log.h"
#include "wayland/state.h"

namespace wlclip::clipboard {

bool parse_backend_kind(std::string_view name, BackendKind& out) {
    if (name.empty() || name == "auto") { out = BackendKind::Auto;        return true; }
    if (name == "wlr" || name == "data-control" ||
        name == "zwlr_data_control_v1" ||
        name == "zwlr_data_control_unstable_v1") {
        out = BackendKind::DataControl; return true;
    }
    if (name == "wl"  || name == "data-device"  ||
        name == "wl_data_device_manager") {
        out = BackendKind::DataDevice;  return true;
    }
    return false;
}

const char* backend_kind_names() {
    return "auto | wlr|data-control|zwlr_data_control_v1 | wl|data-device|wl_data_device_manager";
}

std::unique_ptr<Backend> make_backend(wayland::State& state, BackendKind kind) {
    switch (kind) {
        case BackendKind::DataControl:
            if (!state.data_control_manager()) {
                spdlog::error("requested 'wlr_data_control' but compositor "
                              "does not expose zwlr_data_control_manager_v1");
                return nullptr;
            }
            return std::make_unique<DataControlBackend>(state);

        case BackendKind::DataDevice:
            if (!state.data_device_manager()) {
                spdlog::error("requested 'wl_data_device' but compositor "
                              "does not expose wl_data_device_manager");
                return nullptr;
            }
            return std::make_unique<DataDeviceBackend>(state);

        case BackendKind::Auto:
        default:
            if (state.data_control_manager())
                return std::make_unique<DataControlBackend>(state);
            if (state.data_device_manager())
                return std::make_unique<DataDeviceBackend>(state);
            spdlog::error("compositor exposes neither zwlr_data_control_v1 "
                          "nor wl_data_device_manager");
            return nullptr;
    }
}

}  // namespace wlclip::clipboard
