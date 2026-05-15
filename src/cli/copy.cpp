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

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace wlclip::cli {

namespace {

struct CopyOpts {
    std::vector<std::string> mime_types;  // -t
    bool oneshot      = false;            // -o
    bool trim_newline = false;            // -n
    bool clear        = false;            // -c
    bool foreground   = false;            // -f
    std::vector<std::string> rest;
};

void print_usage() {
    std::cerr <<
        "Usage: wlclip [-P PROTOCOL] [-p] copy [options] [data...]\n"
        "  -t, --type MIME       Advertise MIME (repeatable).\n"
        "  -n, --trim-newline    Strip trailing newline.\n"
        "  -o, --oneshot         Serve a single consumer and exit.\n"
        "  -c, --clear           Clear the selection.\n"
        "  -f, --foreground      Stay foreground (default forks).\n"
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
        bool ok = backend.copy_acquire_then_detach(
            common.primary ? clipboard::Selection::Primary
                           : clipboard::Selection::Regular,
            std::move(data),
            opts.oneshot,
            /*detach=*/!opts.foreground);
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
