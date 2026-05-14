#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_data_device_manager;
struct zwlr_data_control_manager_v1;

namespace wlclip::wayland {

struct SeatInfo {
    wl_seat*      proxy = nullptr;
    std::uint32_t name = 0;
    std::uint32_t version = 0;
    std::string   identifier;     // wl_seat.name event payload
};

struct GlobalInfo {
    std::uint32_t name;
    std::string   interface;
    std::uint32_t version;
};

// Owns a wl_display connection and discovers seats + clipboard managers.
class State {
public:
    State();
    ~State();
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    bool connect(const std::string& display_name = "");

    // Two roundtrips: collect globals + bind, then drain seat-name events.
    void initial_sync();

    wl_display*  display()  const { return display_; }
    wl_registry* registry() const { return registry_; }

    const std::vector<GlobalInfo>& globals() const { return globals_; }
    const std::vector<SeatInfo>&   seats()   const { return seats_; }

    zwlr_data_control_manager_v1* data_control_manager() const { return zwlr_manager_; }
    wl_data_device_manager*       data_device_manager()  const { return wl_manager_; }
    std::uint32_t data_control_manager_version() const { return zwlr_manager_version_; }

    const SeatInfo* pick_seat(const std::string& selector) const;

    bool roundtrip();
    bool dispatch();
    bool flush();

private:
    static const struct wl_registry_listener registry_listener_;
    static const struct wl_seat_listener     seat_listener_;
    static void on_global(void*, wl_registry*, std::uint32_t, const char*, std::uint32_t);
    static void on_global_remove(void*, wl_registry*, std::uint32_t);
    static void on_seat_capabilities(void*, wl_seat*, std::uint32_t);
    static void on_seat_name(void*, wl_seat*, const char*);

    wl_display*  display_  = nullptr;
    wl_registry* registry_ = nullptr;

    std::vector<GlobalInfo> globals_;
    std::vector<SeatInfo>   seats_;

    zwlr_data_control_manager_v1* zwlr_manager_ = nullptr;
    std::uint32_t zwlr_manager_version_ = 0;
    std::uint32_t zwlr_manager_global_name_ = 0;

    wl_data_device_manager* wl_manager_ = nullptr;
    std::uint32_t wl_manager_version_ = 0;
    std::uint32_t wl_manager_global_name_ = 0;
};

}
