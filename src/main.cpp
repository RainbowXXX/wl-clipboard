#include "cli/commands.h"
#include "cli/options.h"
#include "core/log.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    wlclip::cli::CommonOptions opts;
    std::string command;
    std::vector<std::string> rest;
    auto r = wlclip::cli::parse_global_options(args, opts, command, rest);
    if (r.exit_now) return r.exit_code;

    auto level = spdlog::level::info;
    if (opts.quiet)             level = spdlog::level::err;
    else if (opts.verbosity >= 2) level = spdlog::level::trace;
    else if (opts.verbosity == 1) level = spdlog::level::debug;
    wlclip::core::init_logging(level);

    if (command.empty()) {
        wlclip::cli::print_top_usage();
        return 2;
    }
    if (command == "copy")      return wlclip::cli::run_copy(opts, rest);
    if (command == "paste")     return wlclip::cli::run_paste(opts, rest);
    if (command == "protocols") return wlclip::cli::run_protocols(opts, rest);

    std::cerr << "wlclip: unknown command: " << command << "\n";
    wlclip::cli::print_top_usage();
    return 2;
}
