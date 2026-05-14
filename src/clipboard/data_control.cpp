#include "clipboard/data_control.h"

#include "clipboard/transfer.h"
#include "core/fd.h"
#include "core/log.h"
#include "core/mime.h"
#include "wayland/state.h"

#include <wayland-client.h>
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <cstdint>
#include <memory>

namespace wlclip::clipboard {

namespace {

// --- Copy side ---------------------------------------------------------

struct CopyState {
    CopyData data;
    bool oneshot   = false;
    bool done      = false;
    bool cancelled = false;
    int  served    = 0;
};

void on_source_send(void* data, zwlr_data_control_source_v1*,
                    const char* mime_type, std::int32_t fd) {
    auto* st = static_cast<CopyState*>(data);
    spdlog::info("[wlr] clipboard fetched: mime='{}' bytes={} fd={} (served #{})",
                 mime_type, st->data.bytes.size(), fd, st->served + 1);
    detail::serve_send(fd, st->data.bytes.data(), st->data.bytes.size());
    st->served++;
    if (st->oneshot) st->done = true;
}

void on_source_cancelled(void* data, zwlr_data_control_source_v1*) {
    auto* st = static_cast<CopyState*>(data);
    spdlog::debug("data_control source: cancelled (served={})", st->served);
    st->cancelled = true;
    st->done = true;
}

const zwlr_data_control_source_v1_listener source_listener = {
    &on_source_send,
    &on_source_cancelled,
};

// --- Paste side --------------------------------------------------------

struct PendingOffer {
    zwlr_data_control_offer_v1* proxy = nullptr;
    std::vector<std::string>    mimes;
};

struct PasteState {
    std::vector<std::unique_ptr<PendingOffer>> offers;
    PendingOffer* current_regular = nullptr;
    PendingOffer* current_primary = nullptr;
    bool got_regular = false;
    bool got_primary = false;
    bool finished    = false;
};

void on_offer_mime(void* data, zwlr_data_control_offer_v1* proxy, const char* m) {
    auto* st = static_cast<PasteState*>(data);
    for (auto& o : st->offers) if (o->proxy == proxy) {
        o->mimes.emplace_back(m); return;
    }
}
const zwlr_data_control_offer_v1_listener offer_listener = { &on_offer_mime };

PendingOffer* find_offer(PasteState* st, zwlr_data_control_offer_v1* p) {
    if (!p) return nullptr;
    for (auto& o : st->offers) if (o->proxy == p) return o.get();
    return nullptr;
}

void on_device_data_offer(void* data, zwlr_data_control_device_v1*,
                          zwlr_data_control_offer_v1* p) {
    auto* st = static_cast<PasteState*>(data);
    auto e = std::make_unique<PendingOffer>();
    e->proxy = p;
    zwlr_data_control_offer_v1_add_listener(p, &offer_listener, st);
    st->offers.push_back(std::move(e));
}
void on_device_selection(void* data, zwlr_data_control_device_v1*,
                         zwlr_data_control_offer_v1* p) {
    auto* st = static_cast<PasteState*>(data);
    st->current_regular = find_offer(st, p);
    st->got_regular = true;
}
void on_device_primary_selection(void* data, zwlr_data_control_device_v1*,
                                 zwlr_data_control_offer_v1* p) {
    auto* st = static_cast<PasteState*>(data);
    st->current_primary = find_offer(st, p);
    st->got_primary = true;
}
void on_device_finished(void* data, zwlr_data_control_device_v1*) {
    auto* st = static_cast<PasteState*>(data);
    spdlog::debug("data_control device finished");
    st->finished = true;
}
const zwlr_data_control_device_v1_listener device_listener = {
    &on_device_data_offer,
    &on_device_selection,
    &on_device_finished,
    &on_device_primary_selection,
};

}  // namespace

bool DataControlBackend::copy(const wayland::SeatInfo& seat, Selection sel,
                              CopyData data, bool oneshot) {
    auto* mgr = state_.data_control_manager();
    if (!mgr) {
        spdlog::error("zwlr_data_control_manager_v1 not available");
        return false;
    }
    if (sel == Selection::Primary && state_.data_control_manager_version() < 2) {
        spdlog::error("compositor's zwlr_data_control v{} does not support "
                      "primary selection", state_.data_control_manager_version());
        return false;
    }
    if (data.mime_types.empty()) {
        spdlog::error("copy: no MIME types to advertise");
        return false;
    }

    auto* device = zwlr_data_control_manager_v1_get_data_device(mgr, seat.proxy);
    auto* source = zwlr_data_control_manager_v1_create_data_source(mgr);
    CopyState st;
    st.data = std::move(data);
    st.oneshot = oneshot;
    zwlr_data_control_source_v1_add_listener(source, &source_listener, &st);
    for (const auto& m : st.data.mime_types)
        zwlr_data_control_source_v1_offer(source, m.c_str());

    if (sel == Selection::Primary)
        zwlr_data_control_device_v1_set_primary_selection(device, source);
    else
        zwlr_data_control_device_v1_set_selection(device, source);
    state_.flush();
    spdlog::info("offered selection ({} mime type(s)){}",
                 st.data.mime_types.size(), oneshot ? " in oneshot mode" : "");

    while (!st.done) {
        if (!state_.dispatch()) break;
    }

    zwlr_data_control_source_v1_destroy(source);
    zwlr_data_control_device_v1_destroy(device);
    return !st.cancelled || st.served > 0;
}

bool DataControlBackend::paste(const wayland::SeatInfo& seat, Selection sel,
                               const std::string& prefer_mime, bool list_only,
                               PasteResult& out) {
    auto* mgr = state_.data_control_manager();
    if (!mgr) {
        spdlog::error("zwlr_data_control_manager_v1 not available");
        return false;
    }
    if (sel == Selection::Primary && state_.data_control_manager_version() < 2) {
        spdlog::error("compositor's zwlr_data_control v{} does not support "
                      "primary selection", state_.data_control_manager_version());
        return false;
    }

    auto* device = zwlr_data_control_manager_v1_get_data_device(mgr, seat.proxy);
    PasteState st;
    zwlr_data_control_device_v1_add_listener(device, &device_listener, &st);
    state_.flush();

    auto have_target = [&]() {
        if (st.finished) return true;
        return sel == Selection::Primary ? st.got_primary : st.got_regular;
    };
    while (!have_target()) {
        if (!state_.dispatch()) {
            zwlr_data_control_device_v1_destroy(device);
            return false;
        }
    }

    PendingOffer* offer = (sel == Selection::Primary) ? st.current_primary
                                                      : st.current_regular;
    if (!offer) {
        spdlog::warn("no current {} selection",
                     sel == Selection::Primary ? "primary" : "regular");
        zwlr_data_control_device_v1_destroy(device);
        return false;
    }
    out.available = offer->mimes;
    if (list_only) {
        zwlr_data_control_device_v1_destroy(device);
        return true;
    }

    std::string mime = core::choose_mime(offer->mimes, prefer_mime);
    if (mime.empty()) {
        zwlr_data_control_device_v1_destroy(device);
        return false;
    }
    out.mime = mime;

    auto pipe = core::make_pipe();
    if (!pipe.read_end || !pipe.write_end) {
        zwlr_data_control_device_v1_destroy(device);
        return false;
    }
    zwlr_data_control_offer_v1_receive(offer->proxy, mime.c_str(),
                                       pipe.write_end.get());
    state_.flush();
    pipe.write_end.reset();
    out.bytes = core::drain_fd(pipe.read_end.get());
    spdlog::debug("received {} bytes as '{}'", out.bytes.size(), mime);

    for (auto& o : st.offers) if (o->proxy) zwlr_data_control_offer_v1_destroy(o->proxy);
    zwlr_data_control_device_v1_destroy(device);
    return true;
}

}  // namespace wlclip::clipboard
