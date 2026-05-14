#include "wayland/state.h"

#include "core/log.h"

#include <wayland-client.h>
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstring>

namespace wlclip::wayland {

const wl_registry_listener State::registry_listener_ = {
    &State::on_global,
    &State::on_global_remove,
};

const wl_seat_listener State::seat_listener_ = {
    &State::on_seat_capabilities,
    &State::on_seat_name,
};

State::State() = default;

State::~State() {
    for (auto& s : seats_) {
        if (s.proxy) wl_seat_destroy(s.proxy);
    }
    seats_.clear();
    if (zwlr_manager_) zwlr_data_control_manager_v1_destroy(zwlr_manager_);
    if (wl_manager_)   wl_data_device_manager_destroy(wl_manager_);
    if (registry_)     wl_registry_destroy(registry_);
    if (display_)      wl_display_disconnect(display_);
}

bool State::connect(const std::string& display_name) {
    const char* name = display_name.empty() ? nullptr : display_name.c_str();
    display_ = wl_display_connect(name);
    if (!display_) {
        spdlog::error("wl_display_connect failed (display='{}')",
                      display_name.empty() ? "<env>" : display_name);
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener_, this);
    spdlog::debug("connected to Wayland display");
    return true;
}

void State::initial_sync() {
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);
    spdlog::debug("globals={} seats={} wlr_data_control={} wl_data_device={}",
                  globals_.size(), seats_.size(),
                  zwlr_manager_ ? "yes" : "no",
                  wl_manager_ ? "yes" : "no");
}

bool State::roundtrip() {
    if (wl_display_roundtrip(display_) < 0) {
        spdlog::error("wl_display_roundtrip failed");
        return false;
    }
    return true;
}

bool State::dispatch() {
    if (wl_display_dispatch(display_) < 0) {
        spdlog::error("wl_display_dispatch failed");
        return false;
    }
    return true;
}

bool State::flush() {
    if (wl_display_flush(display_) < 0) {
        spdlog::error("wl_display_flush failed");
        return false;
    }
    return true;
}

const SeatInfo* State::pick_seat(const std::string& selector) const {
    if (seats_.empty()) return nullptr;
    if (selector.empty()) return &seats_.front();
    if (std::all_of(selector.begin(), selector.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
        std::size_t idx = static_cast<std::size_t>(std::stoul(selector));
        if (idx < seats_.size()) return &seats_[idx];
    }
    for (const auto& s : seats_) {
        if (s.identifier == selector) return &s;
    }
    return nullptr;
}

void State::on_global(void* data, wl_registry* r,
                      std::uint32_t name, const char* interface,
                      std::uint32_t version) {
    auto* self = static_cast<State*>(data);
    self->globals_.push_back({name, interface, version});

    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        std::uint32_t bv = std::min<std::uint32_t>(version, 7);
        auto* seat = static_cast<wl_seat*>(
            wl_registry_bind(r, name, &wl_seat_interface, bv));
        self->seats_.push_back({seat, name, bv, {}});
        wl_seat_add_listener(seat, &seat_listener_, self);
    } else if (std::strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        std::uint32_t bv = std::min<std::uint32_t>(version, 2);
        self->zwlr_manager_ = static_cast<zwlr_data_control_manager_v1*>(
            wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, bv));
        self->zwlr_manager_version_ = bv;
        self->zwlr_manager_global_name_ = name;
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        std::uint32_t bv = std::min<std::uint32_t>(version, 3);
        self->wl_manager_ = static_cast<wl_data_device_manager*>(
            wl_registry_bind(r, name, &wl_data_device_manager_interface, bv));
        self->wl_manager_version_ = bv;
        self->wl_manager_global_name_ = name;
    }
}

void State::on_global_remove(void* data, wl_registry*, std::uint32_t name) {
    auto* self = static_cast<State*>(data);
    self->globals_.erase(
        std::remove_if(self->globals_.begin(), self->globals_.end(),
                       [name](const GlobalInfo& g) { return g.name == name; }),
        self->globals_.end());

    if (name == self->zwlr_manager_global_name_ && self->zwlr_manager_) {
        spdlog::warn("zwlr_data_control_manager_v1 was removed");
        zwlr_data_control_manager_v1_destroy(self->zwlr_manager_);
        self->zwlr_manager_ = nullptr;
    }
    if (name == self->wl_manager_global_name_ && self->wl_manager_) {
        spdlog::warn("wl_data_device_manager was removed");
        wl_data_device_manager_destroy(self->wl_manager_);
        self->wl_manager_ = nullptr;
    }
}

void State::on_seat_capabilities(void*, wl_seat*, std::uint32_t) {}

void State::on_seat_name(void* data, wl_seat* seat, const char* name) {
    auto* self = static_cast<State*>(data);
    for (auto& s : self->seats_) {
        if (s.proxy == seat) {
            s.identifier = name ? name : "";
            return;
        }
    }
}

}  // namespace wlclip::wayland
