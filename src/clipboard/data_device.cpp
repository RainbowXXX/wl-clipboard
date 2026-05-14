#include "clipboard/data_device.h"

#include "clipboard/transfer.h"
#include "core/fd.h"
#include "core/log.h"
#include "core/mime.h"
#include "wayland/state.h"

#include <wayland-client.h>

#include <cstdint>
#include <memory>
#include <poll.h>

namespace wlclip::clipboard {

namespace {

struct CopyState {
    CopyData data;
    bool oneshot   = false;
    bool done      = false;
    bool cancelled = false;
    int  served    = 0;
};

void on_source_target(void*, wl_data_source*, const char*) {}
void on_source_send(void* data, wl_data_source*, const char* m, std::int32_t fd) {
    auto* st = static_cast<CopyState*>(data);
    spdlog::info("[wl] clipboard fetched: mime='{}' bytes={} fd={} (served #{})",
                 m, st->data.bytes.size(), fd, st->served + 1);
    detail::serve_send(fd, st->data.bytes.data(), st->data.bytes.size());
    st->served++;
    if (st->oneshot) st->done = true;
}
void on_source_cancelled(void* data, wl_data_source*) {
    auto* st = static_cast<CopyState*>(data);
    spdlog::debug("wl_data_source: cancelled (served={})", st->served);
    st->cancelled = true;
    st->done = true;
}
void on_source_dnd_drop_performed(void*, wl_data_source*) {}
void on_source_dnd_finished(void*, wl_data_source*) {}
void on_source_action(void*, wl_data_source*, std::uint32_t) {}

const wl_data_source_listener source_listener = {
    &on_source_target, &on_source_send, &on_source_cancelled,
    &on_source_dnd_drop_performed, &on_source_dnd_finished, &on_source_action,
};

struct PendingOffer {
    wl_data_offer* proxy = nullptr;
    std::vector<std::string> mimes;
};
struct PasteState {
    std::vector<std::unique_ptr<PendingOffer>> offers;
    PendingOffer* current = nullptr;
    bool got_selection = false;
};

void on_offer_offer(void* data, wl_data_offer* p, const char* m) {
    auto* st = static_cast<PasteState*>(data);
    for (auto& o : st->offers) if (o->proxy == p) {
        o->mimes.emplace_back(m); return;
    }
}
void on_offer_source_actions(void*, wl_data_offer*, std::uint32_t) {}
void on_offer_action(void*, wl_data_offer*, std::uint32_t) {}
const wl_data_offer_listener offer_listener = {
    &on_offer_offer, &on_offer_source_actions, &on_offer_action,
};

PendingOffer* find_offer(PasteState* st, wl_data_offer* p) {
    if (!p) return nullptr;
    for (auto& o : st->offers) if (o->proxy == p) return o.get();
    return nullptr;
}
void on_device_data_offer(void* data, wl_data_device*, wl_data_offer* p) {
    auto* st = static_cast<PasteState*>(data);
    auto e = std::make_unique<PendingOffer>();
    e->proxy = p;
    wl_data_offer_add_listener(p, &offer_listener, st);
    st->offers.push_back(std::move(e));
}
void on_device_enter(void*, wl_data_device*, std::uint32_t,
                     wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {}
void on_device_leave(void*, wl_data_device*) {}
void on_device_motion(void*, wl_data_device*, std::uint32_t,
                      wl_fixed_t, wl_fixed_t) {}
void on_device_drop(void*, wl_data_device*) {}
void on_device_selection(void* data, wl_data_device*, wl_data_offer* p) {
    auto* st = static_cast<PasteState*>(data);
    st->current = find_offer(st, p);
    st->got_selection = true;
}
const wl_data_device_listener device_listener = {
    &on_device_data_offer, &on_device_enter, &on_device_leave,
    &on_device_motion, &on_device_drop, &on_device_selection,
};

}  // namespace

bool DataDeviceBackend::copy(const wayland::SeatInfo& seat, Selection sel,
                             CopyData data, bool oneshot) {
    auto* mgr = state_.data_device_manager();
    if (!mgr) {
        spdlog::error("wl_data_device_manager not available");
        return false;
    }
    if (sel == Selection::Primary) {
        spdlog::error("wl_data_device_manager has no primary selection; use "
                      "zwlr_data_control_v1");
        return false;
    }
    spdlog::warn("wl_data_device_manager copy without focus serial is "
                 "expected to be rejected by the compositor");

    auto* device = wl_data_device_manager_get_data_device(mgr, seat.proxy);
    auto* source = wl_data_device_manager_create_data_source(mgr);
    CopyState st;
    st.data = std::move(data);
    st.oneshot = oneshot;
    wl_data_source_add_listener(source, &source_listener, &st);
    for (const auto& m : st.data.mime_types)
        wl_data_source_offer(source, m.c_str());
    // serial=0 is invalid; the compositor will reject this.
    wl_data_device_set_selection(device, source, 0);
    state_.flush();

    while (!st.done) {
        if (!state_.dispatch()) break;
    }
    wl_data_source_destroy(source);
    wl_data_device_destroy(device);
    return !st.cancelled || st.served > 0;
}

bool DataDeviceBackend::paste(const wayland::SeatInfo& seat, Selection sel,
                              const std::string& prefer_mime, bool list_only,
                              PasteResult& out) {
    auto* mgr = state_.data_device_manager();
    if (!mgr) {
        spdlog::error("wl_data_device_manager not available");
        return false;
    }
    if (sel == Selection::Primary) {
        spdlog::error("wl_data_device_manager has no primary selection; use "
                      "zwlr_data_control_v1");
        return false;
    }
    spdlog::warn("wl_data_device_manager paste only sees the focused "
                 "surface's selection; will time out otherwise");

    auto* device = wl_data_device_manager_get_data_device(mgr, seat.proxy);
    PasteState st;
    wl_data_device_add_listener(device, &device_listener, &st);
    state_.flush();

    pollfd pfd{wl_display_get_fd(state_.display()), POLLIN, 0};
    const int wait_ms = 1500;
    while (!st.got_selection) {
        state_.flush();
        if (::poll(&pfd, 1, wait_ms) <= 0) {
            spdlog::error("no selection event within {} ms — focus required", wait_ms);
            wl_data_device_destroy(device);
            return false;
        }
        if (!state_.dispatch()) {
            wl_data_device_destroy(device);
            return false;
        }
    }
    PendingOffer* offer = st.current;
    if (!offer) {
        wl_data_device_destroy(device);
        return false;
    }

    out.available = offer->mimes;
    if (list_only) {
        wl_data_device_destroy(device);
        return true;
    }
    std::string mime = core::choose_mime(offer->mimes, prefer_mime);
    if (mime.empty()) {
        wl_data_device_destroy(device);
        return false;
    }
    out.mime = mime;
    auto pipe = core::make_pipe();
    if (!pipe.read_end || !pipe.write_end) {
        wl_data_device_destroy(device);
        return false;
    }
    wl_data_offer_receive(offer->proxy, mime.c_str(), pipe.write_end.get());
    state_.flush();
    pipe.write_end.reset();
    out.bytes = core::drain_fd(pipe.read_end.get());

    for (auto& o : st.offers) if (o->proxy) wl_data_offer_destroy(o->proxy);
    wl_data_device_destroy(device);
    return true;
}

}  // namespace wlclip::clipboard
