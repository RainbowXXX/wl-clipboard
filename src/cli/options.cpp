#include "cli/options.h"

#include <iostream>
#include <string>

namespace wlclip::cli {

namespace {

bool eat(const std::vector<std::string>& argv, std::size_t& i,
         const char* short_name, const char* long_name,
         std::string* out) {
    const auto& a = argv[i];
    if (a == short_name || a == long_name) {
        if (i + 1 >= argv.size()) return false;
        *out = argv[++i];
        return true;
    }
    std::string prefix = std::string(long_name) + "=";
    if (a.rfind(prefix, 0) == 0) {
        *out = a.substr(prefix.size());
        return true;
    }
    return false;
}

}  // namespace

void print_top_usage() {
    std::cerr <<
        "wlclip — minimal Wayland clipboard tool\n"
        "         (zwlr_data_control_v1 + wl_data_device_manager, no focus hack)\n\n"
        "Usage: wlclip [global options] <command> [command options]\n\n"
        "Commands:\n"
        "  copy        Take ownership of the selection.\n"
        "  paste       Read the current selection to stdout.\n"
        "  protocols   List Wayland globals exposed by the compositor.\n\n"
        "Global options:\n"
        "  -d, --display NAME   Wayland display (default: $WAYLAND_DISPLAY).\n"
        "  -s, --seat ID        Seat index or wl_seat.name (default: first).\n"
        "  -b, --backend B\n"
        "  -P, --protocol B     Clipboard protocol. One of:\n"
        "                         auto (default)\n"
        "                         wlr | data-control | zwlr_data_control_v1\n"
        "                         wl  | data-device  | wl_data_device_manager\n"
        "  -p, --primary        Primary selection.\n"
        "  -v, --verbose        Increase log verbosity (repeatable).\n"
        "  -q, --quiet          Errors only.\n"
        "      --version        Print version and exit.\n"
        "  -h, --help           Show this help.\n";
}

ParseResult parse_global_options(const std::vector<std::string>& argv,
                                 CommonOptions& opts,
                                 std::string& command,
                                 std::vector<std::string>& rest) {
    ParseResult r;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        std::string v;
        if (eat(argv, i, "-d", "--display", &v)) { opts.display = v; continue; }
        if (eat(argv, i, "-s", "--seat",    &v)) { opts.seat    = v; continue; }
        if (eat(argv, i, "-b", "--backend",  &v)) { opts.backend = v; continue; }
        if (eat(argv, i, "-P", "--protocol", &v)) { opts.backend = v; continue; }
        if (a == "-p" || a == "--primary") { opts.primary = true; continue; }
        if (a == "--verbose") { opts.verbosity++; continue; }
        // Accept -v, -vv, -vvv, ... as repeated verbosity bumps.
        if (a.size() >= 2 && a[0] == '-' && a[1] == 'v' &&
            a.find_first_not_of('v', 1) == std::string::npos) {
            opts.verbosity += static_cast<int>(a.size() - 1);
            continue;
        }
        if (a == "-q" || a == "--quiet")   { opts.quiet = true; continue; }
        if (a == "--version") {
            std::cout << "wlclip 0.1.0\n";
            r.exit_now = true; r.exit_code = 0; return r;
        }
        if (a == "-h" || a == "--help" || a == "help") {
            print_top_usage();
            r.exit_now = true; r.exit_code = 0; return r;
        }
        if (!a.empty() && a[0] != '-') {
            command = a;
            for (++i; i < argv.size(); ++i) rest.push_back(argv[i]);
            return r;
        }
        std::cerr << "wlclip: unknown global option: " << a << "\n";
        print_top_usage();
        r.ok = false; r.exit_now = true; r.exit_code = 2; return r;
    }
    return r;
}

}  // namespace wlclip::cli
