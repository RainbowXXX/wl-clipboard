#include "cli/commands.h"

#include "clipboard/backend.h"
#include "clipboard/factory.h"
#include "clipboard/handcrafted.h"
#include "clipboard/x11.h"
#include "core/fd.h"
#include "core/log.h"
#include "core/mime.h"
#include "wayland/state.h"

#include <iostream>
#include <unistd.h>

namespace wlclip::cli {

namespace {

struct PasteOpts {
    std::string type;
    bool list_types = false;
    bool no_newline = false;
};

void print_usage() {
    std::cerr <<
        "Usage: wlclip [-P PROTOCOL] [-p] paste [options]\n"
        "  -t, --type MIME       Receive this MIME exactly.\n"
        "  -l, --list-types      List offered MIME types.\n"
        "  -n, --no-newline      Do not append trailing newline.\n"
        "\n"
        "Protocol selection (global options, before 'paste'):\n"
        "  -P, --protocol PROTO  auto (default) | wlr | wl\n"
        "  -p, --primary         Read the primary selection.\n";
}

bool parse(const std::vector<std::string>& args, PasteOpts& o) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "-t" || a == "--type") {
            if (i + 1 >= args.size()) { spdlog::error("--type needs a value"); return false; }
            o.type = args[++i];
        } else if (a == "-l" || a == "--list-types") o.list_types = true;
        else if (a == "-n" || a == "--no-newline")   o.no_newline = true;
        else if (a == "-h" || a == "--help") { print_usage(); return false; }
        else { spdlog::error("unknown argument: {}", a); print_usage(); return false; }
    }
    return true;
}

}  // namespace

int run_paste(const CommonOptions& common, const std::vector<std::string>& args) {
    PasteOpts opts;
    if (!parse(args, opts)) return 2;

    clipboard::BackendKind kind;
    if (!clipboard::parse_backend_kind(common.backend, kind)) {
        spdlog::error("unknown protocol '{}'. Expected: {}",
                      common.backend, clipboard::backend_kind_names());
        return 2;
    }

    clipboard::PasteResult result;
    bool ok = false;

    if (kind == clipboard::BackendKind::HandcraftedControl) {
        // No libwayland connection on this path.
        clipboard::HandcraftedBackend backend(common.display, common.seat);
        wayland::SeatInfo dummy{};
        spdlog::info("using protocol: {}", backend.name());
        ok = backend.paste(dummy,
                           common.primary ? clipboard::Selection::Primary
                                          : clipboard::Selection::Regular,
                           opts.type, opts.list_types, result);
    } else if (kind == clipboard::BackendKind::X11) {
        clipboard::X11Backend backend(common.display, common.seat);
        wayland::SeatInfo dummy{};
        spdlog::info("using protocol: {}", backend.name());
        ok = backend.paste(dummy,
                           common.primary ? clipboard::Selection::Primary
                                          : clipboard::Selection::Regular,
                           opts.type, opts.list_types, result);
    } else {
        wayland::State ws;
        if (!ws.connect(common.display)) return 1;
        ws.initial_sync();
        const auto* seat = ws.pick_seat(common.seat);
        if (!seat) { spdlog::error("no usable seat"); return 1; }
        spdlog::debug("seat='{}'", seat->identifier);

        auto backend = clipboard::make_backend(ws, kind, common.display, common.seat);
        if (!backend) return 1;
        spdlog::info("using protocol: {}", backend->name());
        ok = backend->paste(*seat,
                            common.primary ? clipboard::Selection::Primary
                                           : clipboard::Selection::Regular,
                            opts.type, opts.list_types, result);
    }
    if (!ok) return 1;

    if (opts.list_types) {
        for (const auto& m : result.available) std::cout << m << '\n';
        return 0;
    }

    if (!result.bytes.empty() &&
        !core::write_all(STDOUT_FILENO, result.bytes.data(), result.bytes.size())) {
        return 1;
    }
    if (!opts.no_newline && core::is_textual(result.mime)) {
        bool has_trailing = !result.bytes.empty() &&
                            result.bytes.back() == std::byte{'\n'};
        if (!has_trailing) {
            const char nl = '\n';
            ::write(STDOUT_FILENO, &nl, 1);
        }
    }
    return 0;
}

}  // namespace wlclip::cli
