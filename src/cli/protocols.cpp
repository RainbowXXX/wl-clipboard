#include "cli/commands.h"

#include "core/log.h"
#include "wayland/state.h"

#include <algorithm>
#include <iostream>

namespace wlclip::cli {

int run_protocols(const CommonOptions& common, const std::vector<std::string>& args) {
    bool verbose = false;
    for (const auto& a : args) {
        if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-h" || a == "--help") {
            std::cerr << "Usage: wlclip protocols [-v]\n"
                         "Lists all Wayland globals advertised by the compositor.\n";
            return 0;
        } else {
            spdlog::error("unknown argument: {}", a);
            return 2;
        }
    }

    wayland::State ws;
    if (!ws.connect(common.display)) return 1;
    ws.initial_sync();

    auto globals = ws.globals();
    std::sort(globals.begin(), globals.end(),
              [](const wayland::GlobalInfo& a, const wayland::GlobalInfo& b) {
                  if (a.interface != b.interface) return a.interface < b.interface;
                  return a.name < b.name;
              });

    if (verbose) {
        std::cout << "interface\tversion\tname\n";
        for (const auto& g : globals)
            std::cout << g.interface << '\t' << g.version << '\t' << g.name << '\n';
    } else {
        for (const auto& g : globals)
            std::cout << g.interface << " v" << g.version << '\n';
    }
    std::cout.flush();

    spdlog::info("zwlr_data_control_manager_v1: {}",
                 ws.data_control_manager() ? "available" : "absent");
    spdlog::info("wl_data_device_manager:       {}",
                 ws.data_device_manager() ? "available" : "absent");
    spdlog::info("seats: {}", ws.seats().size());
    for (const auto& s : ws.seats()) {
        spdlog::info("  - '{}' (wl_seat v{}, global #{})",
                     s.identifier, s.version, s.name);
    }
    return 0;
}

}  // namespace wlclip::cli
