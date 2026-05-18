#include "clipboard/x11.h"

#include "core/log.h"
#include "core/mime.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace wlclip::clipboard {

namespace {

// ---- Atom cache --------------------------------------------------------

struct Atoms {
    xcb_atom_t CLIPBOARD     = XCB_ATOM_NONE;
    xcb_atom_t PRIMARY       = XCB_ATOM_PRIMARY;
    xcb_atom_t TARGETS       = XCB_ATOM_NONE;
    xcb_atom_t TIMESTAMP     = XCB_ATOM_NONE;
    xcb_atom_t MULTIPLE      = XCB_ATOM_NONE;
    xcb_atom_t INCR          = XCB_ATOM_NONE;
    xcb_atom_t UTF8_STRING   = XCB_ATOM_NONE;
    xcb_atom_t STRING        = XCB_ATOM_STRING;
    xcb_atom_t TEXT          = XCB_ATOM_NONE;
    xcb_atom_t WL_SELECTION  = XCB_ATOM_NONE;  // private property for receive
    xcb_atom_t NET_WM_NAME   = XCB_ATOM_NONE;  // used by describe_window logging
};

xcb_atom_t intern_atom(xcb_connection_t* c, std::string_view name) {
    auto cookie = xcb_intern_atom(c, 0, static_cast<std::uint16_t>(name.size()),
                                  name.data());
    auto* reply = xcb_intern_atom_reply(c, cookie, nullptr);
    if (!reply) return XCB_ATOM_NONE;
    xcb_atom_t a = reply->atom;
    std::free(reply);
    return a;
}

std::string atom_name(xcb_connection_t* c, xcb_atom_t atom) {
    if (atom == XCB_ATOM_NONE) return {};
    auto cookie = xcb_get_atom_name(c, atom);
    auto* reply = xcb_get_atom_name_reply(c, cookie, nullptr);
    if (!reply) return {};
    int len = xcb_get_atom_name_name_length(reply);
    std::string s(xcb_get_atom_name_name(reply), len);
    std::free(reply);
    return s;
}

// Batch all InternAtom requests before collecting any reply. XCB pipelines
// the cookies into one server write and we pay a single round-trip for the
// whole set instead of one per atom (8 -> 1 on the copy critical path).
void load_atoms(xcb_connection_t* c, Atoms& a) {
    struct Pending { xcb_atom_t* slot; xcb_intern_atom_cookie_t cookie; };
    auto send = [&](xcb_atom_t* slot, std::string_view name) {
        return Pending{slot,
            xcb_intern_atom(c, 0, static_cast<std::uint16_t>(name.size()),
                            name.data())};
    };
    Pending pending[] = {
        send(&a.CLIPBOARD,    "CLIPBOARD"),
        send(&a.TARGETS,      "TARGETS"),
        send(&a.TIMESTAMP,    "TIMESTAMP"),
        send(&a.MULTIPLE,     "MULTIPLE"),
        send(&a.INCR,         "INCR"),
        send(&a.UTF8_STRING,  "UTF8_STRING"),
        send(&a.TEXT,         "TEXT"),
        send(&a.WL_SELECTION, "WLCLIP_SELECTION"),
        send(&a.NET_WM_NAME,  "_NET_WM_NAME"),
    };
    for (auto& p : pending) {
        auto* reply = xcb_intern_atom_reply(c, p.cookie, nullptr);
        *p.slot = reply ? reply->atom : XCB_ATOM_NONE;
        std::free(reply);
    }
}

xcb_atom_t selection_atom(const Atoms& a, Selection sel) {
    return sel == Selection::Primary ? a.PRIMARY : a.CLIPBOARD;
}

// ---- Connection wrapper ------------------------------------------------

struct X11Connection {
    xcb_connection_t* conn       = nullptr;
    xcb_screen_t*     screen     = nullptr;
    xcb_window_t      window     = 0;
    xcb_timestamp_t   owner_time = XCB_CURRENT_TIME;  // real server time used in SetSelectionOwner
    Atoms             atoms;

    ~X11Connection() {
        if (window && conn) xcb_destroy_window(conn, window);
        if (conn) xcb_disconnect(conn);
    }

    bool open(const std::string& display) {
        int screen_num = 0;
        conn = xcb_connect(display.empty() ? nullptr : display.c_str(),
                           &screen_num);
        if (xcb_connection_has_error(conn)) {
            spdlog::error("xcb_connect failed (display='{}')",
                          display.empty() ? "<env>" : display);
            conn = nullptr;
            return false;
        }
        const xcb_setup_t* setup = xcb_get_setup(conn);
        auto it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < screen_num && it.rem; ++i) xcb_screen_next(&it);
        screen = it.data;
        if (!screen) {
            spdlog::error("no screen #{} on display", screen_num);
            return false;
        }
        load_atoms(conn, atoms);

        // Unmapped 1x1 InputOutput window — never visible, mirrors what
        // xclip creates with XCreateSimpleWindow. Tried InputOnly first
        // (legal per ICCCM and lighter) but measured ~150ms extra latency on
        // KDE/Plasma — klipper / kwin's X-W bridge appear to treat InputOnly
        // owners as utility windows and add a debounce.
        window = xcb_generate_id(conn);
        std::uint32_t mask = XCB_CW_EVENT_MASK;
        std::uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT,
                          window, screen->root,
                          0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          XCB_COPY_FROM_PARENT,
                          mask, values);
        // Set WM_CLASS. Empirically this didn't move the needle much on KDE
        // (within noise across runs), but xclip-style "fully anonymous" was
        // measured slightly slower in one test, so we err on the side of
        // identifying the window. Format is instance\0class\0.
        static const char kWmClass[] = "wlclip\0wlclip";
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window,
                            XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            sizeof(kWmClass) - 1, kWmClass);
        xcb_flush(conn);
        spdlog::debug("x11: connected, window=0x{:x}", window);
        return true;
    }
};

// ---- Targets advertisement --------------------------------------------

struct AdvertisedTargets {
    std::vector<xcb_atom_t>  atoms;
    std::vector<std::string> names;  // parallel to atoms
};

AdvertisedTargets build_targets(xcb_connection_t* c, const Atoms& a,
                                const std::vector<std::string>& mimes) {
    AdvertisedTargets t;
    auto add = [&](xcb_atom_t atom, std::string name) {
        if (atom == XCB_ATOM_NONE) return;
        if (std::find(t.atoms.begin(), t.atoms.end(), atom) != t.atoms.end()) return;
        t.atoms.push_back(atom);
        t.names.push_back(std::move(name));
    };
    add(a.TARGETS, "TARGETS");

    // For text: advertise ONLY the X11-canonical atoms (UTF8_STRING, STRING,
    // TEXT). Skip wayland-style "text/plain;charset=utf-8" / "text/plain"
    // even if the caller put them on the MIME list — kwin / klipper's X-W
    // bridge eagerly ConvertSelections every advertised target, so each
    // extra synonym is one more bridge round-trip and one more cache
    // bookkeeping step before the new selection becomes visible to wayland.
    // The bridge maps UTF8_STRING -> text/plain;charset=utf-8 for wayland
    // consumers automatically, so we lose nothing by not advertising the
    // long form here. This mirrors xclip, which advertises {TARGETS,
    // UTF8_STRING} for text (xclib.c:420-421) and reaches ~90ms steady
    // state vs ~250ms for the fully-spelled-out list.
    //
    // Also do NOT advertise TIMESTAMP. We use CurrentTime in
    // SetSelectionOwner so owner_time is 0, which is an invalid ICCCM
    // timestamp; if a strict consumer queries TIMESTAMP and gets 0 it may
    // refuse the selection or debounce. xclip doesn't advertise it either,
    // so neither do we.
    bool has_text = false;
    std::vector<std::string>              to_intern;     // non-text MIME names
    std::vector<xcb_intern_atom_cookie_t> to_intern_ck;  // parallel cookies
    for (const auto& m : mimes) {
        if (core::is_textual(m)) {
            has_text = true;
        } else {
            to_intern_ck.push_back(xcb_intern_atom(
                c, 0, static_cast<std::uint16_t>(m.size()), m.data()));
            to_intern.push_back(m);
        }
    }
    for (std::size_t i = 0; i < to_intern.size(); ++i) {
        auto* reply = xcb_intern_atom_reply(c, to_intern_ck[i], nullptr);
        xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
        std::free(reply);
        add(atom, to_intern[i]);
    }
    if (has_text) {
        // Advertise ONLY UTF8_STRING for text. Mirrors xclip's default
        // behaviour (xclib.c:420-421 -> Atom types[2] = { targets, target }).
        //
        // Clipboard managers (klipper / dde-clipboard / kwin's X-W bridge)
        // appear to wait for ConvertSelection on every advertised target to
        // complete before they'll commit the new selection to the wayland
        // data_control device. Each extra text variant we advertise turns
        // into one more synchronous round-trip in the manager's pipeline
        // BEFORE the new value becomes visible to wayland clients. Going
        // from {TARGETS, UTF8_STRING, STRING, TEXT} (4) down to {TARGETS,
        // UTF8_STRING} (2) is the difference between staying above 150ms
        // and matching xclip's ~90ms on KDE/dde.
        //
        // Modern consumers all understand UTF8_STRING. STRING (Latin-1) is
        // a legacy fallback that no current text consumer relies on, and
        // TEXT is the "compound text" Lisp-machine relic that's been dead
        // for a decade. xclip has shipped without advertising either since
        // forever.
        add(a.UTF8_STRING, "UTF8_STRING");
    }
    return t;
}

// Find which of our MIMEs to serve for a requested target atom.
std::string match_target(const AdvertisedTargets& t,
                         const std::vector<std::string>& mimes,
                         xcb_atom_t target, const Atoms& a) {
    auto first_textual = [&]() -> std::string {
        for (const auto& m : mimes) if (core::is_textual(m)) return m;
        return {};
    };
    if (target == a.UTF8_STRING || target == a.STRING || target == a.TEXT) {
        return first_textual();
    }
    for (std::size_t i = 0; i < t.atoms.size(); ++i) {
        if (t.atoms[i] == target) {
            if (t.names[i] == "TARGETS" || t.names[i] == "TIMESTAMP") return {};
            return t.names[i];
        }
    }
    return {};
}

// Look up "WM_CLASS" (or fall back to "_NET_WM_NAME"/"WM_NAME") of a window
// so we can label the requester in logs. Returns "0x<id>" if nothing found.
//
// Hot-path constraints:
//  - Caller must invoke this AFTER the SelectionNotify has been flushed, never
//    before. Each round-trip here would otherwise block the requestor.
//  - Atoms are all known up front (predefined or pre-interned in load_atoms),
//    so zero InternAtom round-trips.
//  - The three GetProperty requests are pipelined: send all cookies, then
//    collect replies — one round-trip for all three instead of up to three.
std::string describe_window(xcb_connection_t* c, const Atoms& a, xcb_window_t w) {
    auto class_cookie = xcb_get_property(c, 0, w, XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING, 0, 64);
    auto net_cookie   = xcb_get_property(c, 0, w, a.NET_WM_NAME,
                                         a.UTF8_STRING, 0, 64);
    auto name_cookie  = xcb_get_property(c, 0, w, XCB_ATOM_WM_NAME,
                                         XCB_ATOM_STRING, 0, 64);

    auto take = [&](xcb_get_property_cookie_t cookie) -> std::string {
        std::string out;
        auto* reply = xcb_get_property_reply(c, cookie, nullptr);
        if (!reply) return out;
        int len = xcb_get_property_value_length(reply);
        if (len > 0) {
            out.assign(static_cast<const char*>(xcb_get_property_value(reply)),
                       static_cast<std::size_t>(len));
        }
        std::free(reply);
        return out;
    };

    // WM_CLASS is two NUL-separated strings: instance \0 class \0
    std::string wm_class = take(class_cookie);
    std::string net_name = take(net_cookie);
    std::string wm_name  = take(name_cookie);

    if (!wm_class.empty()) {
        auto sep = wm_class.find('\0');
        std::string inst = wm_class.substr(0, sep);
        std::string cls  = (sep != std::string::npos)
                         ? wm_class.substr(sep + 1)
                         : std::string();
        while (!cls.empty()  && cls.back()  == '\0') cls.pop_back();
        while (!inst.empty() && inst.back() == '\0') inst.pop_back();
        if (!cls.empty() || !inst.empty()) {
            return cls.empty() ? inst : (inst.empty() ? cls : inst + "/" + cls);
        }
    }
    if (!net_name.empty()) return net_name;
    if (!wm_name.empty())  return wm_name;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%x", w);
    return buf;
}

void send_selection_notify(xcb_connection_t* c,
                           xcb_selection_request_event_t* req,
                           xcb_atom_t prop) {
    xcb_selection_notify_event_t ev{};
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time      = req->time;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target    = req->target;
    ev.property  = prop;          // None = refused
    xcb_send_event(c, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&ev));
}

// Request `target` and wait for SelectionNotify on x.window. Returns true
// when the server set our property; the caller then reads it.
bool request_and_wait(X11Connection& x, xcb_atom_t selatom, xcb_atom_t target,
                      int timeout_ms = 2000) {
    xcb_convert_selection(x.conn, x.window, selatom, target,
                          x.atoms.WL_SELECTION, XCB_CURRENT_TIME);
    xcb_flush(x.conn);

    auto start = std::chrono::steady_clock::now();
    while (true) {
        xcb_flush(x.conn);
        if (auto* ev = xcb_poll_for_event(x.conn)) {
            std::unique_ptr<xcb_generic_event_t, decltype(&std::free)>
                guard(ev, &std::free);
            std::uint8_t t = ev->response_type & ~0x80;
            if (t == XCB_SELECTION_NOTIFY) {
                auto* sn = reinterpret_cast<xcb_selection_notify_event_t*>(ev);
                if (sn->property == XCB_ATOM_NONE) {
                    spdlog::debug("x11 paste: target refused by owner");
                    return false;
                }
                return true;
            }
            continue;
        }
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();
        if (el >= timeout_ms) {
            spdlog::error("x11 paste: SelectionNotify timeout ({} ms)", timeout_ms);
            return false;
        }
        pollfd pfd{xcb_get_file_descriptor(x.conn), POLLIN, 0};
        ::poll(&pfd, 1, std::max<int>(0, timeout_ms - el));
    }
}

// Read the WL_SELECTION property into a byte vector. The property is also
// deleted from the server (delete=1).
std::vector<std::byte> read_property(X11Connection& x, xcb_atom_t* type_out) {
    std::vector<std::byte> data;
    std::uint32_t offset = 0;
    while (true) {
        auto cookie = xcb_get_property(x.conn, /*delete=*/1, x.window,
                                       x.atoms.WL_SELECTION,
                                       XCB_GET_PROPERTY_TYPE_ANY,
                                       offset, 1024 * 1024 /* up to 4 MiB */);
        auto* reply = xcb_get_property_reply(x.conn, cookie, nullptr);
        if (!reply) {
            spdlog::error("x11 paste: get_property failed");
            return data;
        }
        if (type_out) *type_out = reply->type;
        if (reply->type == x.atoms.INCR) {
            std::uint32_t expected = 0;
            if (xcb_get_property_value_length(reply) >= 4)
                std::memcpy(&expected, xcb_get_property_value(reply), 4);
            spdlog::warn("x11 paste: INCR transfer not implemented; "
                         "expected ~{} bytes — bailing out", expected);
            std::free(reply);
            return {};
        }
        int len = xcb_get_property_value_length(reply);
        if (len > 0) {
            auto* p = static_cast<const std::byte*>(xcb_get_property_value(reply));
            data.insert(data.end(), p, p + len);
        }
        bool more = reply->bytes_after > 0;
        offset += static_cast<std::uint32_t>(len) / 4;
        std::free(reply);
        if (!more) break;
    }
    return data;
}

}  // namespace

// ====== Copy ============================================================

namespace {

// Everything we need to keep alive across the fork boundary in the
// xclip-style "acquire-then-detach" flow.
struct CopyContext {
    X11Connection     x;
    AdvertisedTargets targets;
    xcb_atom_t        selatom = XCB_ATOM_NONE;
    CopyData          data;
};

// Phase 1: open connection, build TARGETS list, acquire and verify selection
// ownership. Must complete *before* the parent process exits in detached mode,
// so that outside observers (e.g. Xwayland → wayland compositor) can already
// see the new X11 owner by the time the shell command returns.
bool acquire_selection(const std::string& display, Selection sel,
                       CopyData&& data, CopyContext& ctx) {
    if (data.mime_types.empty()) {
        spdlog::error("x11 copy: no MIME types");
        return false;
    }
    if (!ctx.x.open(display)) return false;

    ctx.selatom = selection_atom(ctx.x.atoms, sel);
    ctx.targets = build_targets(ctx.x.conn, ctx.x.atoms, data.mime_types);

    // Use CurrentTime to mirror xclip exactly. We previously fetched a real
    // server timestamp via a PropertyNotify trick, intending to dodge a
    // hypothetical "time == 0" debounce in some compositors — measurements on
    // KDE/Plasma showed the opposite: CurrentTime takes the fast path and the
    // real-timestamp path adds ~150ms to the time the new selection becomes
    // visible to wl-paste. ICCCM §2.1 prefers a real time but xclip has shipped
    // with CurrentTime forever; we match that. The TIMESTAMP target reply will
    // be 0, which all observed consumers (Xwayland, klipper, kwin) tolerate.
    ctx.x.owner_time = XCB_CURRENT_TIME;
    xcb_set_selection_owner(ctx.x.conn, ctx.x.window, ctx.selatom,
                            XCB_CURRENT_TIME);
    xcb_flush(ctx.x.conn);

    auto own_c = xcb_get_selection_owner(ctx.x.conn, ctx.selatom);
    auto* own_r = xcb_get_selection_owner_reply(ctx.x.conn, own_c, nullptr);
    if (!own_r || own_r->owner != ctx.x.window) {
        spdlog::error("x11 copy: failed to become selection owner");
        if (own_r) std::free(own_r);
        return false;
    }
    std::free(own_r);
    spdlog::info("x11: own {} selection (mimes={})",
                 sel == Selection::Primary ? "PRIMARY" : "CLIPBOARD",
                 data.mime_types.size());

    ctx.data = std::move(data);
    return true;
}

// Phase 2: block in the X event loop, answering SelectionRequests until we
// lose ownership (or until oneshot mode serves one consumer and exits).
bool serve_selection(CopyContext& ctx, bool oneshot) {
    auto&       x       = ctx.x;
    const auto& targets = ctx.targets;
    const auto& data    = ctx.data;
    const auto  selatom = ctx.selatom;

    int  served = 0;
    bool lost   = false;
    while (!lost) {
        xcb_flush(x.conn);
        auto* ev = xcb_wait_for_event(x.conn);
        if (!ev) {
            spdlog::warn("x11 copy: connection closed");
            break;
        }
        std::unique_ptr<xcb_generic_event_t, decltype(&std::free)>
            ev_guard(ev, &std::free);
        std::uint8_t t = ev->response_type & ~0x80;

        if (t == XCB_SELECTION_REQUEST) {
            // Stopwatch covers everything the requestor (Xwayland) is blocked
            // on: from the moment we picked the SelectionRequest up off the
            // socket until our SelectionNotify is flushed back. This is the
            // number to watch when chasing copy->paste latency.
            auto req_start = std::chrono::steady_clock::now();

            auto* req = reinterpret_cast<xcb_selection_request_event_t*>(ev);
            if (req->selection != selatom) {
                send_selection_notify(x.conn, req, XCB_ATOM_NONE);
                xcb_flush(x.conn);
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - req_start).count();
                spdlog::info("[x11] refused foreign selection in {}.{:03} ms",
                             us / 1000, us % 1000);
                continue;
            }
            xcb_atom_t prop = req->property ? req->property : req->target;

            // ---- Phase A: build and send the response IMMEDIATELY ----
            // Anything that needs an X round-trip (atom_name / describe_window)
            // is deferred to Phase B. Doing logging first would stall the
            // requestor (Xwayland) for the whole duration of those round-trips
            // — which is what makes a wlclip paste appear ~150ms slower than
            // xclip in end-to-end timings.
            enum class Kind { Targets, Timestamp, Served, Refused };
            Kind        kind;
            std::string served_mime;

            if (req->target == x.atoms.TARGETS) {
                xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                    req->requestor, prop, XCB_ATOM_ATOM, 32,
                                    static_cast<std::uint32_t>(targets.atoms.size()),
                                    targets.atoms.data());
                send_selection_notify(x.conn, req, prop);
                kind = Kind::Targets;
            } else if (req->target == x.atoms.TIMESTAMP) {
                std::uint32_t ts = x.owner_time;
                xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                    req->requestor, prop, XCB_ATOM_INTEGER, 32,
                                    1, &ts);
                send_selection_notify(x.conn, req, prop);
                kind = Kind::Timestamp;
            } else {
                served_mime = match_target(targets, data.mime_types,
                                           req->target, x.atoms);
                if (served_mime.empty()) {
                    send_selection_notify(x.conn, req, XCB_ATOM_NONE);
                    kind = Kind::Refused;
                } else {
                    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                        req->requestor, prop, req->target, 8,
                                        static_cast<std::uint32_t>(data.bytes.size()),
                                        data.bytes.data());
                    send_selection_notify(x.conn, req, prop);
                    served++;
                    if (oneshot) lost = true;
                    kind = Kind::Served;
                }
            }
            xcb_flush(x.conn);  // <-- requestor unblocked here
            auto req_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - req_start).count();
            // ms.us as integer pair so the format is "12.345 ms" without
            // pulling in floating-point or locale.
            long long ms_part = req_us / 1000;
            long long us_part = req_us % 1000;

            // ---- Phase B: logging only, all expensive XCB calls live here ----
            const bool want_info  = spdlog::should_log(spdlog::level::info);
            const bool want_debug = spdlog::should_log(spdlog::level::debug);
            switch (kind) {
            case Kind::Targets:
                if (want_info) {
                    spdlog::info("[x11] TARGETS queried by '{}' (win 0x{:x}) "
                                 "served in {}.{:03} ms",
                                 describe_window(x.conn, x.atoms, req->requestor),
                                 req->requestor, ms_part, us_part);
                }
                break;
            case Kind::Timestamp:
                if (want_debug) {
                    spdlog::debug("[x11] TIMESTAMP queried by '{}' "
                                  "served in {}.{:03} ms",
                                  describe_window(x.conn, x.atoms, req->requestor),
                                  ms_part, us_part);
                }
                break;
            case Kind::Refused:
                if (want_info) {
                    spdlog::info("[x11] refused target '{}' from '{}' (win 0x{:x}) "
                                 "in {}.{:03} ms",
                                 atom_name(x.conn, req->target),
                                 describe_window(x.conn, x.atoms, req->requestor),
                                 req->requestor, ms_part, us_part);
                }
                break;
            case Kind::Served:
                if (want_info) {
                    spdlog::info("[x11] clipboard fetched by '{}' (win 0x{:x}): "
                                 "mime='{}' target='{}' bytes={} (served #{}) "
                                 "in {}.{:03} ms",
                                 describe_window(x.conn, x.atoms, req->requestor),
                                 req->requestor, served_mime,
                                 atom_name(x.conn, req->target),
                                 data.bytes.size(), served, ms_part, us_part);
                }
                break;
            }
        } else if (t == XCB_SELECTION_CLEAR) {
            spdlog::debug("x11: selection cleared (served={})", served);
            lost = true;
        }
    }
    return served > 0 || !lost;
}

}  // namespace

bool X11Backend::copy(const wayland::SeatInfo&, Selection sel,
                      CopyData data, bool oneshot) {
    // No fork — caller (or the test harness) decides lifecycle.
    return copy_acquire_then_detach(sel, std::move(data), oneshot,
                                    /*detach=*/false);
}

bool X11Backend::copy_acquire_then_detach(Selection sel, CopyData data,
                                          bool oneshot, bool detach) {
    CopyContext ctx;
    if (!acquire_selection(display_, sel, std::move(data), ctx)) {
        return false;
    }

    if (detach) {
        // Match xclip's flow: ownership is acquired+verified in the parent,
        // *then* fork. The shell sees the command return only after Xwayland
        // and the compositor can already see the new X11 owner.
        pid_t pid = ::fork();
        if (pid < 0) {
            spdlog::error("fork() failed: {}", std::strerror(errno));
            return false;
        }
        if (pid > 0) {
            // Parent: bail out without unwinding. The X11Connection destructor
            // would xcb_destroy_window (releasing ownership we just took) and
            // xcb_disconnect (closing the shared X socket). _exit skips all
            // C++ destructors and atexit handlers — this is what xclip does.
            ::_exit(0);
        }
        // Child becomes the daemon.
        ::setsid();
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            if (devnull > 2) ::close(devnull);
        }
    }

    return serve_selection(ctx, oneshot);
}

// ====== Paste ===========================================================

bool X11Backend::paste(const wayland::SeatInfo&, Selection sel,
                       const std::string& prefer_mime, bool list_only,
                       PasteResult& out) {
    X11Connection x;
    if (!x.open(display_)) return false;
    xcb_atom_t selatom = selection_atom(x.atoms, sel);

    // 1. Enumerate offered targets.
    auto t_targets = std::chrono::steady_clock::now();
    if (!request_and_wait(x, selatom, x.atoms.TARGETS)) {
        spdlog::error("x11 paste: no {} owner or TARGETS unsupported",
                      sel == Selection::Primary ? "PRIMARY" : "CLIPBOARD");
        return false;
    }
    xcb_atom_t type = 0;
    auto raw = read_property(x, &type);
    auto targets_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t_targets).count();
    spdlog::info("[x11] paste TARGETS roundtrip: {}.{:03} ms",
                 targets_us / 1000, targets_us % 1000);
    if (raw.empty() || (raw.size() % 4) != 0) {
        spdlog::error("x11 paste: TARGETS property empty/invalid");
        return false;
    }
    std::vector<xcb_atom_t> tatoms(raw.size() / 4);
    std::memcpy(tatoms.data(), raw.data(), raw.size());

    // Translate atoms → MIME names. Classic text atoms get canonical strings.
    std::vector<std::string> mimes;
    for (auto a : tatoms) {
        if (a == XCB_ATOM_NONE) continue;
        if (a == x.atoms.UTF8_STRING)      mimes.emplace_back("text/plain;charset=utf-8");
        else if (a == x.atoms.STRING)      mimes.emplace_back("text/plain");
        else if (a == x.atoms.TEXT)        mimes.emplace_back("TEXT");
        else if (a == x.atoms.TARGETS ||
                 a == x.atoms.TIMESTAMP ||
                 a == x.atoms.MULTIPLE)    continue;
        else                               mimes.emplace_back(atom_name(x.conn, a));
    }
    out.available = mimes;
    if (list_only) return true;

    std::string mime = core::choose_mime(mimes, prefer_mime);
    if (mime.empty()) {
        spdlog::error("x11 paste: owner offers no usable MIME");
        return false;
    }

    // Map chosen MIME → target atom.
    xcb_atom_t target = XCB_ATOM_NONE;
    if      (mime == "text/plain;charset=utf-8") target = x.atoms.UTF8_STRING;
    else if (mime == "text/plain")               target = x.atoms.STRING;
    else if (mime == "TEXT")                     target = x.atoms.TEXT;
    else                                         target = intern_atom(x.conn, mime);
    if (target == XCB_ATOM_NONE) {
        spdlog::error("x11 paste: couldn't intern atom for '{}'", mime);
        return false;
    }

    auto t_data = std::chrono::steady_clock::now();
    if (!request_and_wait(x, selatom, target)) {
        spdlog::error("x11 paste: data convert refused");
        return false;
    }
    out.bytes = read_property(x, nullptr);
    auto data_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t_data).count();
    out.mime  = mime;
    spdlog::info("[x11] paste received: mime='{}' bytes={} in {}.{:03} ms",
                 mime, out.bytes.size(), data_us / 1000, data_us % 1000);
    return true;
}

}  // namespace wlclip::clipboard
