#include "cli/commands.h"

#include "clipboard/backend.h"
#include "clipboard/factory.h"
#include "clipboard/handcrafted.h"
#include "clipboard/x11.h"
#include "core/fd.h"
#include "core/log.h"
#include "core/mime.h"
#include "wayland/state.h"

#include <wayland-client.h>
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <unistd.h>

namespace wlclip::cli {

namespace {

struct CopyOpts {
    std::vector<std::string> mime_types;  // -t
    bool oneshot      = false;            // -o
    bool trim_newline = false;            // -n
    bool clear        = false;            // -c
    bool foreground   = false;            // -f
    bool sync_wait    = false;            // -s / --sync
    int  sync_timeout_ms = 500;           // --sync-timeout
    std::vector<std::string> rest;
};

void print_usage() {
    std::cerr <<
        "Usage: wlclip [-P PROTOCOL] [-p] copy [options] [data...]\n"
        "  -t, --type MIME           Advertise MIME (repeatable).\n"
        "  -n, --trim-newline        Strip trailing newline.\n"
        "  -o, --oneshot             Serve a single consumer and exit.\n"
        "  -c, --clear               Clear the selection.\n"
        "  -f, --foreground          Stay foreground (default forks).\n"
        "  -s, --sync                X11 only: don't return until the new\n"
        "                            selection is visible on the wayland\n"
        "                            data_control device AND a receive() of\n"
        "                            it returns the exact bytes we set.\n"
        "                            Adds ~30-100ms to copy latency; lets\n"
        "                            callers skip any post-copy polling.\n"
        "      --sync-timeout MS     Sync wait deadline (default 500).\n"
        "\n"
        "Protocol selection (global options, before 'copy'):\n"
        "  -P, --protocol PROTO  auto (default) | wlr | wl\n"
        "  -p, --primary         Operate on the primary selection.\n";
}

bool parse(const std::vector<std::string>& args, CopyOpts& o) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&](const char* what) -> const std::string* {
            if (i + 1 >= args.size()) {
                spdlog::error("option {} requires an argument", what);
                return nullptr;
            }
            return &args[++i];
        };
        if (a == "-t" || a == "--type") {
            auto* v = next("--type"); if (!v) return false;
            o.mime_types.push_back(*v);
        } else if (a == "-n" || a == "--trim-newline") o.trim_newline = true;
        else if (a == "-o" || a == "--oneshot")        o.oneshot = true;
        else if (a == "-c" || a == "--clear")          o.clear = true;
        else if (a == "-f" || a == "--foreground")     o.foreground = true;
        else if (a == "-s" || a == "--sync")           o.sync_wait = true;
        else if (a == "--sync-timeout") {
            auto* v = next("--sync-timeout"); if (!v) return false;
            try {
                o.sync_timeout_ms = std::stoi(*v);
            } catch (const std::exception&) {
                spdlog::error("--sync-timeout: not an integer: '{}'", *v);
                return false;
            }
            if (o.sync_timeout_ms < 0) o.sync_timeout_ms = 0;
        }
        else if (a == "-h" || a == "--help") { print_usage(); return false; }
        else if (a == "--") { for (++i; i < args.size(); ++i) o.rest.push_back(args[i]); }
        else if (!a.empty() && a[0] == '-') {
            spdlog::error("unknown option: {}", a); print_usage(); return false;
        } else {
            o.rest.push_back(a);
        }
    }
    return true;
}

std::vector<std::byte> collect_payload(const CopyOpts& o) {
    std::vector<std::byte> bytes;
    if (!o.rest.empty()) {
        for (std::size_t i = 0; i < o.rest.size(); ++i) {
            const auto& s = o.rest[i];
            bytes.insert(bytes.end(),
                         reinterpret_cast<const std::byte*>(s.data()),
                         reinterpret_cast<const std::byte*>(s.data()) + s.size());
            if (i + 1 < o.rest.size()) bytes.push_back(std::byte{' '});
        }
        bytes.push_back(std::byte{'\n'});
    } else {
        bytes = core::drain_fd(STDIN_FILENO);
    }
    if (o.trim_newline && !bytes.empty() && bytes.back() == std::byte{'\n'}) {
        bytes.pop_back();
    }
    return bytes;
}

bool detach_to_background() {
    pid_t pid = ::fork();
    if (pid < 0) { spdlog::error("fork(): {}", std::strerror(errno)); return false; }
    if (pid > 0) ::_exit(0);
    ::setsid();
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        if (devnull > 2) ::close(devnull);
    }
    return true;
}

// =========================================================================
// --sync verify: after X11 SetSelectionOwner, open a separate wayland
// connection and wait for the wlr_data_control device to expose a selection
// whose advertised MIMEs include something we set AND whose bytes match what
// we set. Returns true on confirmed visibility.
//
// Soft-failure semantics: any setup problem (no zwlr_data_control, no seat,
// timeout, …) returns false with a warning but does NOT propagate as an
// error to the caller. The X selection is set regardless; sync is a
// best-effort "wait until visible" guarantee, not a precondition.
//
// Must be called AFTER fork() in the parent process: the receive() round-trip
// goes compositor → Xwayland → ConvertSelection → X owner, and the X owner
// in this design is the child (which is concurrently executing
// serve_selection). Calling this before fork would deadlock — there'd be
// nobody to service the SelectionRequest while we sit waiting on
// drain_fd().
// =========================================================================
bool wait_for_wl_visibility(
    const std::string& display,
    bool primary,
    const std::vector<std::string>& my_mimes,
    const std::vector<std::byte>& my_bytes,
    int timeout_ms) {

    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();
    const auto deadline = t_start + std::chrono::milliseconds(timeout_ms);
    auto remaining_ms = [&] {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - clock::now()).count();
        return rem > 0 ? static_cast<int>(rem) : 0;
    };

    wayland::State ws;
    if (!ws.connect(display)) {
        spdlog::warn("[x11 sync] wayland connect failed; selection is set "
                     "on X but visibility couldn't be verified");
        return false;
    }
    ws.initial_sync();

    auto* mgr = ws.data_control_manager();
    if (!mgr) {
        spdlog::warn("[x11 sync] compositor doesn't advertise "
                     "zwlr_data_control_manager_v1; can't verify");
        return false;
    }
    if (primary && ws.data_control_manager_version() < 2) {
        spdlog::warn("[x11 sync] data_control v{} doesn't support primary; "
                     "can't verify", ws.data_control_manager_version());
        return false;
    }
    const auto* seat = ws.pick_seat("");
    if (!seat) {
        spdlog::warn("[x11 sync] no wl_seat available; can't verify");
        return false;
    }

    struct OfferRec {
        zwlr_data_control_offer_v1* proxy = nullptr;
        std::vector<std::string>    mimes;
    };
    struct SyncState {
        std::vector<std::unique_ptr<OfferRec>> offers;
        zwlr_data_control_offer_v1* current_regular = nullptr;
        zwlr_data_control_offer_v1* current_primary = nullptr;
        bool fresh_regular = false;
        bool fresh_primary = false;
    };
    SyncState st;

    static const zwlr_data_control_offer_v1_listener offer_listener = {
        // offer
        [](void* data, zwlr_data_control_offer_v1* proxy, const char* mime) {
            auto* s = static_cast<SyncState*>(data);
            for (auto& o : s->offers) {
                if (o->proxy == proxy) {
                    o->mimes.emplace_back(mime);
                    spdlog::info("[x11 sync trace] offer {} +mime '{}'",
                                 static_cast<void*>(proxy), mime);
                    return;
                }
            }
        },
    };

    static const zwlr_data_control_device_v1_listener device_listener = {
        // data_offer
        [](void* data, zwlr_data_control_device_v1*,
           zwlr_data_control_offer_v1* p) {
            auto* s = static_cast<SyncState*>(data);
            auto r = std::make_unique<OfferRec>();
            r->proxy = p;
            zwlr_data_control_offer_v1_add_listener(p, &offer_listener, s);
            s->offers.push_back(std::move(r));
            spdlog::info("[x11 sync trace] new data_offer {}",
                         static_cast<void*>(p));
        },
        // selection
        [](void* data, zwlr_data_control_device_v1*,
           zwlr_data_control_offer_v1* p) {
            auto* s = static_cast<SyncState*>(data);
            s->current_regular = p;
            s->fresh_regular = true;
            spdlog::info("[x11 sync trace] selection event -> offer={}",
                         static_cast<void*>(p));
        },
        // finished
        [](void*, zwlr_data_control_device_v1*) {
            spdlog::info("[x11 sync trace] device finished");
        },
        // primary_selection
        [](void* data, zwlr_data_control_device_v1*,
           zwlr_data_control_offer_v1* p) {
            auto* s = static_cast<SyncState*>(data);
            s->current_primary = p;
            s->fresh_primary = true;
            spdlog::info("[x11 sync trace] primary_selection event -> offer={}",
                         static_cast<void*>(p));
        },
    };

    auto* device = zwlr_data_control_manager_v1_get_data_device(mgr, seat->proxy);
    zwlr_data_control_device_v1_add_listener(device, &device_listener, &st);
    ws.flush();
    spdlog::info("[x11 sync trace] device created, listener attached; "
                 "my_mimes=[{}] my_bytes={} primary={}",
                 [&]{ std::string s; for (auto& m : my_mimes) { if(!s.empty())s+=","; s+=m; } return s; }(),
                 my_bytes.size(), primary);

    int fd = wl_display_get_fd(ws.display());

    // Probe the *current* selection offer: receive() its bytes and compare.
    // Returns 1=match, 0=mismatch-or-not-yet, -1=hard fail.
    //
    // We do NOT gate this on a "fresh selection event" anymore. Reason:
    // dde-clipboard (and likely klipper too) keeps the same wayland source
    // object across X-side selection changes. When the X selection changes,
    // dde-clipboard updates its INTERNAL cache but does NOT re-call
    // set_selection on wlr_data_control, so wayland clients never receive a
    // new selection event. The only way to observe the cache refresh is to
    // re-receive() on the same offer and compare bytes.
    auto probe_current_offer = [&]() -> int {
        auto*& current = primary ? st.current_primary : st.current_regular;
        if (!current) return 0;

        OfferRec* offer = nullptr;
        for (auto& o : st.offers) {
            if (o->proxy == current) { offer = o.get(); break; }
        }
        if (!offer) return 0;

        std::string mime;
        for (const auto& m : my_mimes) {
            if (std::find(offer->mimes.begin(), offer->mimes.end(), m)
                != offer->mimes.end()) { mime = m; break; }
        }
        if (mime.empty()) return 0;

        auto pipe = core::make_pipe();
        if (!pipe.read_end || !pipe.write_end) return -1;
        zwlr_data_control_offer_v1_receive(offer->proxy, mime.c_str(),
                                           pipe.write_end.get());
        ws.flush();
        pipe.write_end.reset();
        auto t_recv = clock::now();
        auto got = core::drain_fd(pipe.read_end.get());
        auto recv_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           clock::now() - t_recv).count();
        bool ok = (got.size() == my_bytes.size()) &&
                  (my_bytes.empty() ||
                   std::memcmp(got.data(), my_bytes.data(), got.size()) == 0);
        spdlog::debug("[x11 sync trace] probe offer={} mime='{}' got={}b "
                      "want={}b in {}ms -> {}",
                      static_cast<void*>(offer->proxy), mime,
                      got.size(), my_bytes.size(), recv_ms,
                      ok ? "MATCH" : "stale");
        return ok ? 1 : 0;
    };

    // Drain initial state: the get_data_device call above causes the
    // compositor to synchronously enumerate the current offers + selection
    // before we start polling.
    ws.roundtrip();
    spdlog::debug("[x11 sync trace] post-roundtrip: offers={} cur_reg={} cur_pri={}",
                  st.offers.size(),
                  static_cast<void*>(st.current_regular),
                  static_cast<void*>(st.current_primary));

    // Probe-retry loop. Each iteration: probe the current offer, then either
    // declare match or wait briefly for events / time for the source's cache
    // to refresh. We retry even WITHOUT a wayland event — clipboard
    // managers tend to update an existing source's cache silently when the
    // X selection changes underneath, which we can only detect by another
    // receive().
    //
    // Per-iteration cost: ~5-15ms (one receive + drain round-trip) +
    // up to `probe_interval_ms` waiting. With ~30ms interval and 500ms
    // budget that's ~10-12 probes, plenty to catch the cache refresh.
    constexpr int probe_interval_ms = 30;
    bool verified = false;
    while (!verified) {
        int r = probe_current_offer();
        if (r == 1) { verified = true; break; }
        if (r < 0)  break;

        int rem = remaining_ms();
        if (rem <= 0) break;

        // Wait up to probe_interval_ms for an event; if none, fall through
        // and probe again anyway.
        int wait_ms = std::min(probe_interval_ms, rem);
        ws.flush();
        pollfd pfd{fd, POLLIN, 0};
        int p = ::poll(&pfd, 1, wait_ms);
        if (p < 0)  { spdlog::debug("[x11 sync trace] poll errno={}", errno); break; }
        if (p > 0) {
            if (wl_display_dispatch(ws.display()) < 0) {
                spdlog::debug("[x11 sync trace] dispatch error");
                break;
            }
        }
        // p == 0: poll timeout, no events. Loop and re-probe.
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          clock::now() - t_start).count();

    for (auto& o : st.offers) {
        if (o->proxy) zwlr_data_control_offer_v1_destroy(o->proxy);
    }
    zwlr_data_control_device_v1_destroy(device);

    if (verified) {
        spdlog::info("[x11 sync] verified ({} bytes) in {} ms",
                     my_bytes.size(), elapsed_ms);
    } else {
        spdlog::warn("[x11 sync] timeout after {} ms (X selection IS set; "
                     "wayland side just hasn't reflected it yet)", elapsed_ms);
    }
    return verified;
}

int handle_clear(wayland::State& ws, const wayland::SeatInfo& seat, bool primary) {
    auto* mgr = ws.data_control_manager();
    if (!mgr) {
        spdlog::error("--clear requires zwlr_data_control_v1");
        return 1;
    }
    auto* dev = zwlr_data_control_manager_v1_get_data_device(mgr, seat.proxy);
    if (primary && ws.data_control_manager_version() >= 2) {
        zwlr_data_control_device_v1_set_primary_selection(dev, nullptr);
    } else {
        zwlr_data_control_device_v1_set_selection(dev, nullptr);
    }
    ws.flush();
    ws.roundtrip();
    zwlr_data_control_device_v1_destroy(dev);
    spdlog::info("selection cleared");
    return 0;
}

}  // namespace

int run_copy(const CommonOptions& common, const std::vector<std::string>& args) {
    CopyOpts opts;
    if (!parse(args, opts)) return 2;

    clipboard::BackendKind kind;
    if (!opts.clear && !clipboard::parse_backend_kind(common.backend, kind)) {
        spdlog::error("unknown protocol '{}'. Expected: {}",
                      common.backend, clipboard::backend_kind_names());
        return 2;
    }

    // ---- foreground phase ----
    // Read stdin (and any other state that depends on the controlling
    // terminal / inherited file descriptors) BEFORE forking. After fork()
    // the daemon will dup stdin/stdout to /dev/null.
    clipboard::CopyData data;
    if (!opts.clear) {
        data.bytes = collect_payload(opts);
        if (opts.mime_types.empty()) {
            // Match wl-clipboard's MIME order: bare text/plain first so that
            // clients that probe types sequentially get a usable answer
            // immediately.
            std::string guessed = core::guess_mime(data.bytes, "");
            if (core::starts_with(guessed, "text/")) {
                data.mime_types.push_back("text/plain;charset=utf-8");
                data.mime_types.push_back("text/plain");
                data.mime_types.push_back("UTF8_STRING");
                data.mime_types.push_back("STRING");
                data.mime_types.push_back("TEXT");
            } else {
                data.mime_types.push_back(std::move(guessed));
            }
        } else {
            data.mime_types = opts.mime_types;
        }
    }

    // ---- X11 (no libwayland) fast path ----
    // Handled BEFORE the generic detach_to_background() below because the X11
    // backend needs to acquire+verify selection ownership in the *parent*
    // process and only fork afterwards (xclip-style). Forking first and then
    // doing the X handshake in a setsid'd orphan adds ~150ms of latency
    // between "shell command returns" and "Xwayland/compositor sees the new
    // X11 owner", which any outside observer polling the clipboard will see.
    if (kind == clipboard::BackendKind::X11) {
        if (opts.clear) {
            spdlog::error("--clear is not implemented for the X11 backend");
            return 1;
        }
        clipboard::X11Backend backend(common.display, common.seat);
        spdlog::info("using protocol: {}", backend.name());

        // --sync only makes sense when we're going to fork. In foreground
        // mode the caller is by definition holding the terminal, so there's
        // no shell command to "return" — they can just paste themselves.
        if (opts.sync_wait && opts.foreground) {
            spdlog::warn("--sync ignored: --foreground is set, so there's no "
                         "fork to delay anyway");
        }
        const bool do_sync = opts.sync_wait && !opts.foreground;

        // Snapshot what we need for the verify callback before `data` gets
        // moved into the backend.
        std::vector<std::string>     my_mimes = data.mime_types;
        std::vector<std::byte>       my_bytes = data.bytes;
        const std::string&           disp     = common.display;
        const bool                   primary  = common.primary;
        const int                    sync_to  = opts.sync_timeout_ms;

        std::function<void()> after_fork;
        if (do_sync) {
            after_fork = [disp, primary, my_mimes = std::move(my_mimes),
                          my_bytes = std::move(my_bytes), sync_to]() {
                wait_for_wl_visibility(disp, primary, my_mimes, my_bytes, sync_to);
            };
        }

        bool ok = backend.copy_acquire_then_detach(
            primary ? clipboard::Selection::Primary
                    : clipboard::Selection::Regular,
            std::move(data),
            opts.oneshot,
            /*detach=*/!opts.foreground,
            std::move(after_fork));
        return ok ? 0 : 1;
    }

    // Fork BEFORE connecting to Wayland. Forking with an already-open
    // wl_display copies its internal state (buffers, mutex, ID counter,
    // socket fd) into the child, which is fragile and observably breaks
    // selection forwarding to wl_data_device consumers (e.g. GTK apps
    // like gedit). Connecting after fork gives the daemon a clean,
    // independent connection.
    if (!opts.clear && !opts.foreground && !detach_to_background()) return 1;

    // ---- Handcrafted (no libwayland) fast path ----
    if (kind == clipboard::BackendKind::HandcraftedControl) {
        if (opts.clear) {
            spdlog::error("--clear is not implemented for the handcrafted backend");
            return 1;
        }
        clipboard::HandcraftedBackend backend(common.display, common.seat);
        wayland::SeatInfo dummy{};
        spdlog::info("using protocol: {}", backend.name());
        bool ok = backend.copy(dummy,
                               common.primary ? clipboard::Selection::Primary
                                              : clipboard::Selection::Regular,
                               std::move(data),
                               opts.oneshot);
        return ok ? 0 : 1;
    }

    // ---- libwayland background phase ----
    wayland::State ws;
    if (!ws.connect(common.display)) return 1;
    ws.initial_sync();

    const auto* seat = ws.pick_seat(common.seat);
    if (!seat) {
        spdlog::error("no usable seat (requested='{}', available={})",
                      common.seat, ws.seats().size());
        return 1;
    }
    spdlog::debug("seat='{}'", seat->identifier);

    if (opts.clear) return handle_clear(ws, *seat, common.primary);

    auto backend = clipboard::make_backend(ws, kind, common.display, common.seat);
    if (!backend) return 1;
    spdlog::info("using protocol: {}", backend->name());

    bool ok = backend->copy(*seat,
                            common.primary ? clipboard::Selection::Primary
                                           : clipboard::Selection::Regular,
                            std::move(data),
                            opts.oneshot);
    return ok ? 0 : 1;
}

}  // namespace wlclip::cli
