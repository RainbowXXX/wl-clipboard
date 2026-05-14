#include "clipboard/x11.h"

#include "core/log.h"
#include "core/mime.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
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

void load_atoms(xcb_connection_t* c, Atoms& a) {
    a.CLIPBOARD    = intern_atom(c, "CLIPBOARD");
    a.TARGETS      = intern_atom(c, "TARGETS");
    a.TIMESTAMP    = intern_atom(c, "TIMESTAMP");
    a.MULTIPLE     = intern_atom(c, "MULTIPLE");
    a.INCR         = intern_atom(c, "INCR");
    a.UTF8_STRING  = intern_atom(c, "UTF8_STRING");
    a.TEXT         = intern_atom(c, "TEXT");
    a.WL_SELECTION = intern_atom(c, "WLCLIP_SELECTION");
}

xcb_atom_t selection_atom(const Atoms& a, Selection sel) {
    return sel == Selection::Primary ? a.PRIMARY : a.CLIPBOARD;
}

// ---- Connection wrapper ------------------------------------------------

struct X11Connection {
    xcb_connection_t* conn   = nullptr;
    xcb_screen_t*     screen = nullptr;
    xcb_window_t      window = 0;
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

        // Unmapped 1x1 InputOnly window — never visible, legal owner/requestor.
        window = xcb_generate_id(conn);
        std::uint32_t mask = XCB_CW_EVENT_MASK;
        std::uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT,
                          window, screen->root,
                          0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT,
                          mask, values);
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
    add(a.TARGETS,   "TARGETS");
    add(a.TIMESTAMP, "TIMESTAMP");
    bool has_text = false;
    for (const auto& m : mimes) {
        if (core::is_textual(m)) has_text = true;
        add(intern_atom(c, m), m);
    }
    if (has_text) {
        add(a.UTF8_STRING, "UTF8_STRING");
        add(a.TEXT,        "TEXT");
        add(a.STRING,      "STRING");
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
std::string describe_window(xcb_connection_t* c, xcb_window_t w) {
    auto fetch = [&](xcb_atom_t prop, xcb_atom_t type, std::uint32_t max_words) {
        std::string out;
        auto cookie = xcb_get_property(c, 0, w, prop, type, 0, max_words);
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
    std::string wm_class = fetch(intern_atom(c, "WM_CLASS"), XCB_ATOM_STRING, 64);
    if (!wm_class.empty()) {
        auto sep = wm_class.find('\0');
        std::string inst = wm_class.substr(0, sep);
        std::string cls  = (sep != std::string::npos)
                         ? wm_class.substr(sep + 1)
                         : std::string();
        // strip trailing NUL chars
        while (!cls.empty() && cls.back() == '\0') cls.pop_back();
        while (!inst.empty() && inst.back() == '\0') inst.pop_back();
        if (!cls.empty() || !inst.empty()) {
            return cls.empty() ? inst : (inst.empty() ? cls : inst + "/" + cls);
        }
    }
    std::string net_name = fetch(intern_atom(c, "_NET_WM_NAME"),
                                 intern_atom(c, "UTF8_STRING"), 64);
    if (!net_name.empty()) return net_name;
    std::string wm_name = fetch(XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 64);
    if (!wm_name.empty()) return wm_name;
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

bool X11Backend::copy(const wayland::SeatInfo&, Selection sel,
                      CopyData data, bool oneshot) {
    if (data.mime_types.empty()) {
        spdlog::error("x11 copy: no MIME types");
        return false;
    }
    X11Connection x;
    if (!x.open(display_)) return false;

    xcb_atom_t selatom = selection_atom(x.atoms, sel);
    AdvertisedTargets targets = build_targets(x.conn, x.atoms, data.mime_types);

    xcb_set_selection_owner(x.conn, x.window, selatom, XCB_CURRENT_TIME);
    xcb_flush(x.conn);

    auto own_c = xcb_get_selection_owner(x.conn, selatom);
    auto* own_r = xcb_get_selection_owner_reply(x.conn, own_c, nullptr);
    if (!own_r || own_r->owner != x.window) {
        spdlog::error("x11 copy: failed to become selection owner");
        if (own_r) std::free(own_r);
        return false;
    }
    std::free(own_r);
    spdlog::info("x11: own {} selection (mimes={})",
                 sel == Selection::Primary ? "PRIMARY" : "CLIPBOARD",
                 data.mime_types.size());

    int served = 0;
    bool lost = false;
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
            auto* req = reinterpret_cast<xcb_selection_request_event_t*>(ev);
            if (req->selection != selatom) {
                send_selection_notify(x.conn, req, XCB_ATOM_NONE);
                continue;
            }
            xcb_atom_t prop = req->property ? req->property : req->target;

            std::string who = describe_window(x.conn, req->requestor);

            if (req->target == x.atoms.TARGETS) {
                xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                    req->requestor, prop, XCB_ATOM_ATOM, 32,
                                    static_cast<std::uint32_t>(targets.atoms.size()),
                                    targets.atoms.data());
                send_selection_notify(x.conn, req, prop);
                spdlog::info("[x11] TARGETS queried by '{}' (win 0x{:x})",
                             who, req->requestor);
            } else if (req->target == x.atoms.TIMESTAMP) {
                std::uint32_t ts = XCB_CURRENT_TIME;
                xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                    req->requestor, prop, XCB_ATOM_INTEGER, 32,
                                    1, &ts);
                send_selection_notify(x.conn, req, prop);
                spdlog::debug("[x11] TIMESTAMP queried by '{}'", who);
            } else {
                std::string mime = match_target(targets, data.mime_types,
                                                req->target, x.atoms);
                if (mime.empty()) {
                    spdlog::info("[x11] refused target '{}' from '{}' (win 0x{:x})",
                                 atom_name(x.conn, req->target), who, req->requestor);
                    send_selection_notify(x.conn, req, XCB_ATOM_NONE);
                } else {
                    xcb_change_property(x.conn, XCB_PROP_MODE_REPLACE,
                                        req->requestor, prop, req->target, 8,
                                        static_cast<std::uint32_t>(data.bytes.size()),
                                        data.bytes.data());
                    send_selection_notify(x.conn, req, prop);
                    served++;
                    spdlog::info("[x11] clipboard fetched by '{}' (win 0x{:x}): "
                                 "mime='{}' target='{}' bytes={} (served #{})",
                                 who, req->requestor, mime,
                                 atom_name(x.conn, req->target),
                                 data.bytes.size(), served);
                    if (oneshot) lost = true;
                }
            }
            xcb_flush(x.conn);
        } else if (t == XCB_SELECTION_CLEAR) {
            spdlog::debug("x11: selection cleared (served={})", served);
            lost = true;
        }
    }
    return served > 0 || !lost;
}

// ====== Paste ===========================================================

bool X11Backend::paste(const wayland::SeatInfo&, Selection sel,
                       const std::string& prefer_mime, bool list_only,
                       PasteResult& out) {
    X11Connection x;
    if (!x.open(display_)) return false;
    xcb_atom_t selatom = selection_atom(x.atoms, sel);

    // 1. Enumerate offered targets.
    if (!request_and_wait(x, selatom, x.atoms.TARGETS)) {
        spdlog::error("x11 paste: no {} owner or TARGETS unsupported",
                      sel == Selection::Primary ? "PRIMARY" : "CLIPBOARD");
        return false;
    }
    xcb_atom_t type = 0;
    auto raw = read_property(x, &type);
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

    if (!request_and_wait(x, selatom, target)) {
        spdlog::error("x11 paste: data convert refused");
        return false;
    }
    out.bytes = read_property(x, nullptr);
    out.mime  = mime;
    spdlog::debug("x11 paste: {} bytes as '{}'", out.bytes.size(), mime);
    return true;
}

}  // namespace wlclip::clipboard
