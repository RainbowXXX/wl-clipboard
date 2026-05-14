#include "clipboard/handcrafted.h"

#include "clipboard/transfer.h"
#include "core/fd.h"
#include "core/log.h"
#include "core/mime.h"
#include "wire/wire.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace wlclip::clipboard {

namespace {

// ---- Opcodes (must match the protocol XML / generator output) ----------

namespace wl_display {
    constexpr std::uint16_t kReqSync         = 0;
    constexpr std::uint16_t kReqGetRegistry  = 1;
    constexpr std::uint16_t kEvError         = 0;
    constexpr std::uint16_t kEvDeleteId      = 1;
}
namespace wl_registry {
    constexpr std::uint16_t kReqBind        = 0;
    constexpr std::uint16_t kEvGlobal       = 0;
    constexpr std::uint16_t kEvGlobalRemove = 1;
}
namespace wl_callback {
    constexpr std::uint16_t kEvDone         = 0;
}
namespace wl_seat {
    constexpr std::uint16_t kEvCapabilities = 0;
    constexpr std::uint16_t kEvName         = 1;
}
namespace zdcm {  // zwlr_data_control_manager_v1
    constexpr std::uint16_t kReqCreateDataSource = 0;
    constexpr std::uint16_t kReqGetDataDevice    = 1;
    constexpr std::uint16_t kReqDestroy          = 2;
}
namespace zdcd {  // zwlr_data_control_device_v1
    constexpr std::uint16_t kReqSetSelection         = 0;
    constexpr std::uint16_t kReqDestroy              = 1;
    constexpr std::uint16_t kReqSetPrimarySelection  = 2;
    constexpr std::uint16_t kEvDataOffer             = 0;
    constexpr std::uint16_t kEvSelection             = 1;
    constexpr std::uint16_t kEvFinished              = 2;
    constexpr std::uint16_t kEvPrimarySelection      = 3;
}
namespace zdcs {  // zwlr_data_control_source_v1
    constexpr std::uint16_t kReqOffer     = 0;
    constexpr std::uint16_t kReqDestroy   = 1;
    constexpr std::uint16_t kEvSend       = 0;
    constexpr std::uint16_t kEvCancelled  = 1;
}
namespace zdco {  // zwlr_data_control_offer_v1
    constexpr std::uint16_t kReqReceive = 0;
    constexpr std::uint16_t kReqDestroy = 1;
    constexpr std::uint16_t kEvOffer    = 0;
}

// ---- Interface name strings ---------------------------------------------

constexpr const char* kIfWlSeat   = "wl_seat";
constexpr const char* kIfZwlrMgr  = "zwlr_data_control_manager_v1";

// ---- Client object table -------------------------------------------------

enum class ObjKind {
    Unknown,
    Display,
    Registry,
    Callback,
    Seat,
    Manager,
    Device,
    Source,
    Offer,
};

struct Object {
    ObjKind kind = ObjKind::Unknown;
};

class Client {
public:
    bool connect(const std::string& display) {
        if (!conn_.connect(display)) return false;
        // wl_display is always object 1 — it's implicitly bound.
        objects_[wire::kDisplayId] = {ObjKind::Display};
        return true;
    }

    wire::Connection& conn() { return conn_; }

    std::uint32_t alloc_id() {
        std::uint32_t id = next_id_++;
        return id;
    }

    void register_object(std::uint32_t id, ObjKind kind) {
        objects_[id] = {kind};
    }
    ObjKind kind_of(std::uint32_t id) const {
        auto it = objects_.find(id);
        return it == objects_.end() ? ObjKind::Unknown : it->second.kind;
    }
    void forget(std::uint32_t id) { objects_.erase(id); }

    // Block until at least one full message has been processed by the
    // registered dispatchers, or until `predicate` returns true. Returns
    // false on permanent error.
    bool dispatch_until(const std::function<bool()>& predicate);

    using Dispatcher =
        std::function<void(std::uint32_t object_id, std::uint16_t opcode,
                           wire::Reader& r)>;
    Dispatcher dispatcher;

private:
    wire::Connection conn_;
    std::unordered_map<std::uint32_t, Object> objects_;
    std::uint32_t next_id_ = wire::kClientIdMin;
};

bool Client::dispatch_until(const std::function<bool()>& predicate) {
    while (!predicate()) {
        if (!conn_.flush()) return false;

        // Try to parse messages already buffered first.
        bool consumed_any = false;
        while (true) {
            std::size_t msg_len =
                wire::peek_message_size(conn_.in_data(), conn_.in_size());
            if (msg_len == 0) break;
            const std::uint8_t* buf = conn_.in_data();
            std::uint32_t object_id;
            std::uint32_t op_len;
            std::memcpy(&object_id, buf,     4);
            std::memcpy(&op_len,    buf + 4, 4);
            std::uint16_t opcode = op_len & 0xFFFFu;
            wire::Reader r(object_id, opcode, buf + 8, msg_len - 8);
            if (dispatcher) dispatcher(object_id, opcode, r);
            conn_.in_consume(msg_len);
            consumed_any = true;
            if (predicate()) return true;
        }
        if (consumed_any) continue;

        // Need more bytes.
        if (!conn_.wait_readable(-1)) return false;
        ssize_t n = conn_.pump();
        if (n < 0) return false;
        if (n == 0) {
            spdlog::error("wire: connection closed by compositor");
            return false;
        }
    }
    return true;
}

// ---- Per-call state shared with the dispatcher --------------------------

struct Globals {
    std::uint32_t registry_id = 0;
    bool          sync_done   = false;
    std::uint32_t sync_cb_id  = 0;

    struct SeatGlobal {
        std::uint32_t name;
        std::uint32_t version;
    };
    struct ManagerGlobal {
        std::uint32_t name;
        std::uint32_t version;
    };
    std::vector<SeatGlobal>   seats;
    std::vector<ManagerGlobal> managers;
};

void send_get_registry(Client& cl, Globals& g) {
    g.registry_id = cl.alloc_id();
    cl.register_object(g.registry_id, ObjKind::Registry);
    wire::Writer w(wire::kDisplayId, wl_display::kReqGetRegistry);
    w.u32(g.registry_id);
    auto bytes = w.finalize();
    cl.conn().out_bytes(bytes.data(), bytes.size());
}

std::uint32_t send_sync(Client& cl) {
    std::uint32_t cb = cl.alloc_id();
    cl.register_object(cb, ObjKind::Callback);
    wire::Writer w(wire::kDisplayId, wl_display::kReqSync);
    w.u32(cb);
    auto bytes = w.finalize();
    cl.conn().out_bytes(bytes.data(), bytes.size());
    return cb;
}

// Build a wl_registry.bind request. The `new_id` argument in the bind
// request has the special "polymorphic" encoding:
//     string interface  +  uint32 version  +  uint32 new_id
void send_bind(Client& cl, std::uint32_t registry_id,
               std::uint32_t global_name,
               std::string_view interface,
               std::uint32_t version,
               std::uint32_t new_id) {
    wire::Writer w(registry_id, wl_registry::kReqBind);
    w.u32(global_name);
    w.str(interface);
    w.u32(version);
    w.u32(new_id);
    auto bytes = w.finalize();
    cl.conn().out_bytes(bytes.data(), bytes.size());
}

// ---- Operation state machines -------------------------------------------

struct CopyState {
    Globals  g;
    std::uint32_t seat_id    = 0;
    std::string   seat_name;
    std::uint32_t manager_id = 0;
    std::uint32_t manager_version = 0;
    std::uint32_t device_id  = 0;
    std::uint32_t source_id  = 0;

    CopyData      data;
    bool          oneshot   = false;
    bool          done      = false;
    bool          cancelled = false;
    int           served    = 0;
    int           cancel_count = 0;  // total cancelled events received
};

struct PasteState {
    Globals  g;
    std::uint32_t seat_id    = 0;
    std::string   seat_name;
    std::uint32_t manager_id = 0;
    std::uint32_t manager_version = 0;
    std::uint32_t device_id  = 0;

    struct OfferRec {
        std::uint32_t id = 0;
        std::vector<std::string> mimes;
    };
    std::vector<OfferRec> offers;
    std::uint32_t current_regular = 0;
    std::uint32_t current_primary = 0;
    bool got_regular = false;
    bool got_primary = false;
    bool finished    = false;
};

// ---- Common: dispatch wl_display / wl_registry / wl_callback / wl_seat --

template <typename State>
void handle_common(Client& cl, State& st,
                   std::uint32_t object_id, std::uint16_t opcode,
                   wire::Reader& r) {
    ObjKind k = cl.kind_of(object_id);
    switch (k) {
        case ObjKind::Display: {
            if (opcode == wl_display::kEvError) {
                std::uint32_t bad = r.u32();
                std::uint32_t code = r.u32();
                std::string msg = r.str();
                spdlog::error("wire: protocol error from compositor: "
                              "object {} code {} '{}'", bad, code, msg);
            } else if (opcode == wl_display::kEvDeleteId) {
                std::uint32_t id = r.u32();
                cl.forget(id);
            }
            return;
        }
        case ObjKind::Registry: {
            if (opcode == wl_registry::kEvGlobal) {
                std::uint32_t name = r.u32();
                std::string iface  = r.str();
                std::uint32_t ver  = r.u32();
                if (iface == kIfWlSeat) {
                    st.g.seats.push_back({name, ver});
                } else if (iface == kIfZwlrMgr) {
                    st.g.managers.push_back({name, ver});
                }
            }
            return;
        }
        case ObjKind::Callback: {
            if (opcode == wl_callback::kEvDone) {
                if (object_id == st.g.sync_cb_id) st.g.sync_done = true;
            }
            return;
        }
        case ObjKind::Seat: {
            if (opcode == wl_seat::kEvName) {
                st.seat_name = r.str();
            }
            return;
        }
        default:
            return;
    }
}

// ---- Bootstrap: connect, get globals, pick seat, bind manager -----------

template <typename State>
bool bootstrap(Client& cl, State& st,
               const std::string& seat_selector) {
    send_get_registry(cl, st.g);
    st.g.sync_cb_id = send_sync(cl);
    if (!cl.dispatch_until([&]{ return st.g.sync_done; })) return false;

    if (st.g.managers.empty()) {
        spdlog::error("compositor does not expose zwlr_data_control_manager_v1");
        return false;
    }
    if (st.g.seats.empty()) {
        spdlog::error("no wl_seat globals advertised");
        return false;
    }

    // Pick seat: by integer index or wl_seat.name.
    std::size_t seat_idx = 0;
    if (!seat_selector.empty()) {
        bool numeric = std::all_of(seat_selector.begin(), seat_selector.end(),
                                   [](char c){ return c >= '0' && c <= '9'; });
        if (numeric) {
            seat_idx = static_cast<std::size_t>(std::stoul(seat_selector));
            if (seat_idx >= st.g.seats.size()) {
                spdlog::error("seat index {} out of range ({} seats)",
                              seat_idx, st.g.seats.size());
                return false;
            }
        }
        // Named lookup needs a second pass after binding; defer.
    }

    const auto& sg = st.g.seats[seat_idx];
    st.seat_id = cl.alloc_id();
    cl.register_object(st.seat_id, ObjKind::Seat);
    send_bind(cl, st.g.registry_id, sg.name, kIfWlSeat,
              std::min<std::uint32_t>(sg.version, 7), st.seat_id);

    const auto& mg = st.g.managers.front();
    st.manager_id = cl.alloc_id();
    st.manager_version = std::min<std::uint32_t>(mg.version, 2);
    cl.register_object(st.manager_id, ObjKind::Manager);
    send_bind(cl, st.g.registry_id, mg.name, kIfZwlrMgr,
              st.manager_version, st.manager_id);

    // Roundtrip so wl_seat.name arrives before we proceed.
    st.g.sync_done  = false;
    st.g.sync_cb_id = send_sync(cl);
    if (!cl.dispatch_until([&]{ return st.g.sync_done; })) return false;

    // Named seat selection (after we received seat.name).
    if (!seat_selector.empty() &&
        !std::all_of(seat_selector.begin(), seat_selector.end(),
                     [](char c){ return c >= '0' && c <= '9'; })) {
        if (st.seat_name != seat_selector) {
            spdlog::error("seat '{}' not bound (only first seat is currently "
                          "bound; got '{}')", seat_selector, st.seat_name);
            return false;
        }
    }

    spdlog::debug("handcrafted bootstrap done: seat='{}' manager v{}",
                  st.seat_name, st.manager_version);
    return true;
}

}  // namespace

// ====== Copy =============================================================

bool HandcraftedBackend::copy(const wayland::SeatInfo&, Selection sel,
                              CopyData data, bool oneshot) {
    Client cl;
    if (!cl.connect(display_)) return false;
    CopyState st;
    st.data    = std::move(data);
    st.oneshot = oneshot;

    cl.dispatcher = [&](std::uint32_t obj, std::uint16_t op, wire::Reader& r) {
        ObjKind k = cl.kind_of(obj);
        if (k == ObjKind::Source) {
            if (op == zdcs::kEvSend) {
                std::string mime = r.str();
                int fd = cl.conn().pop_fd();
                spdlog::debug("handcrafted source: send mime='{}' fd={}",
                              mime, fd);
                if (fd >= 0) {
                    detail::serve_send(fd, st.data.bytes.data(),
                                       st.data.bytes.size());
                }
                st.served++;
                if (st.oneshot) st.done = true;
            } else if (op == zdcs::kEvCancelled) {
                st.cancel_count++;
                // First cancelled is treated as potentially spurious: some
                // compositors / clipboard managers have been observed to
                // emit a premature `cancelled` and then call `receive`
                // anyway, which — if we had already torn down the source —
                // would return garbage (e.g. a pipe of NULs). Stay alive
                // through the first one and only honour the second.
                if (st.cancel_count == 1) {
                    spdlog::warn("handcrafted source: cancelled #1 ignored "
                                 "(served={}, staying alive for late receive)",
                                 st.served);
                } else {
                    spdlog::debug("handcrafted source: cancelled #{} "
                                  "(served={}) — exiting",
                                  st.cancel_count, st.served);
                    st.cancelled = true;
                    st.done = true;
                }
            }
            return;
        }
        handle_common(cl, st, obj, op, r);
    };

    if (!bootstrap(cl, st, seat_)) return false;

    if (sel == Selection::Primary && st.manager_version < 2) {
        spdlog::error("compositor's zwlr_data_control v{} does not support "
                      "primary selection", st.manager_version);
        return false;
    }
    if (st.data.mime_types.empty()) {
        spdlog::error("copy: no MIME types to advertise");
        return false;
    }

    // create_data_source
    st.source_id = cl.alloc_id();
    cl.register_object(st.source_id, ObjKind::Source);
    {
        wire::Writer w(st.manager_id, zdcm::kReqCreateDataSource);
        w.u32(st.source_id);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
    }
    // get_data_device(seat)
    st.device_id = cl.alloc_id();
    cl.register_object(st.device_id, ObjKind::Device);
    {
        wire::Writer w(st.manager_id, zdcm::kReqGetDataDevice);
        w.u32(st.device_id);
        w.u32(st.seat_id);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
    }
    // source.offer(mime) for each
    for (const auto& m : st.data.mime_types) {
        wire::Writer w(st.source_id, zdcs::kReqOffer);
        w.str(m);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
    }
    // device.set_selection(source) or set_primary_selection(source)
    {
        wire::Writer w(st.device_id,
                       sel == Selection::Primary ? zdcd::kReqSetPrimarySelection
                                                 : zdcd::kReqSetSelection);
        w.u32(st.source_id);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
    }
    cl.conn().flush();
    spdlog::info("offered selection ({} mime type(s)){} via handcrafted backend",
                 st.data.mime_types.size(), oneshot ? " in oneshot mode" : "");

    bool ok = cl.dispatch_until([&]{ return st.done; });
    return ok && (!st.cancelled || st.served > 0);
}

// ====== Paste ============================================================

bool HandcraftedBackend::paste(const wayland::SeatInfo&, Selection sel,
                               const std::string& prefer_mime, bool list_only,
                               PasteResult& out) {
    Client cl;
    if (!cl.connect(display_)) return false;
    PasteState st;

    cl.dispatcher = [&](std::uint32_t obj, std::uint16_t op, wire::Reader& r) {
        ObjKind k = cl.kind_of(obj);
        if (k == ObjKind::Device) {
            if (op == zdcd::kEvDataOffer) {
                std::uint32_t off = r.u32();
                cl.register_object(off, ObjKind::Offer);
                st.offers.push_back({off, {}});
            } else if (op == zdcd::kEvSelection) {
                std::uint32_t off = r.u32();
                st.current_regular = off;
                st.got_regular = true;
            } else if (op == zdcd::kEvPrimarySelection) {
                std::uint32_t off = r.u32();
                st.current_primary = off;
                st.got_primary = true;
            } else if (op == zdcd::kEvFinished) {
                st.finished = true;
            }
            return;
        }
        if (k == ObjKind::Offer) {
            if (op == zdco::kEvOffer) {
                std::string m = r.str();
                for (auto& o : st.offers) {
                    if (o.id == obj) { o.mimes.push_back(std::move(m)); break; }
                }
            }
            return;
        }
        handle_common(cl, st, obj, op, r);
    };

    if (!bootstrap(cl, st, seat_)) return false;

    if (sel == Selection::Primary && st.manager_version < 2) {
        spdlog::error("compositor's zwlr_data_control v{} does not support "
                      "primary selection", st.manager_version);
        return false;
    }

    // get_data_device(seat)
    st.device_id = cl.alloc_id();
    cl.register_object(st.device_id, ObjKind::Device);
    {
        wire::Writer w(st.manager_id, zdcm::kReqGetDataDevice);
        w.u32(st.device_id);
        w.u32(st.seat_id);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
    }
    cl.conn().flush();

    auto have_target = [&]() {
        if (st.finished) return true;
        return sel == Selection::Primary ? st.got_primary : st.got_regular;
    };
    if (!cl.dispatch_until(have_target)) return false;

    std::uint32_t off_id = (sel == Selection::Primary) ? st.current_primary
                                                        : st.current_regular;
    if (!off_id) {
        spdlog::warn("no current {} selection",
                     sel == Selection::Primary ? "primary" : "regular");
        return false;
    }

    PasteState::OfferRec* rec = nullptr;
    for (auto& o : st.offers) if (o.id == off_id) { rec = &o; break; }
    if (!rec) {
        spdlog::warn("selection refers to unknown offer #{}", off_id);
        return false;
    }
    out.available = rec->mimes;
    if (list_only) return true;

    std::string mime = core::choose_mime(rec->mimes, prefer_mime);
    if (mime.empty()) return false;
    out.mime = mime;

    auto pipe = core::make_pipe();
    if (!pipe.read_end || !pipe.write_end) return false;

    // offer.receive(mime, fd)
    {
        wire::Writer w(off_id, zdco::kReqReceive);
        w.str(mime);
        auto b = w.finalize();
        cl.conn().out_bytes(b.data(), b.size());
        cl.conn().out_fd(pipe.write_end.release());
    }
    cl.conn().flush();
    out.bytes = core::drain_fd(pipe.read_end.get());
    spdlog::debug("handcrafted: received {} bytes as '{}'",
                  out.bytes.size(), mime);
    return true;
}

}  // namespace wlclip::clipboard
