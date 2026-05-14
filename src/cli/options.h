#pragma once

#include <string>
#include <vector>

namespace wlclip::cli {

struct CommonOptions {
    std::string display;        // -d / --display
    std::string seat;           // -s / --seat
    std::string backend;        // -b / --backend (auto|wlr|wl)
    bool        primary   = false;  // -p / --primary
    int         verbosity = 0;       // -v repeated
    bool        quiet     = false;   // -q
};

// Parse the global option prefix from argv (everything before the subcommand
// name). On success, fills `opts`, sets `command` to the subcommand, and
// `rest` to its arguments. Returns false on bad input or when help/version
// was printed.
struct ParseResult {
    bool ok = true;
    bool exit_now = false;          // help / version printed
    int  exit_code = 0;
};

ParseResult parse_global_options(const std::vector<std::string>& argv,
                                 CommonOptions& opts,
                                 std::string& command,
                                 std::vector<std::string>& rest);

void print_top_usage();

}  // namespace wlclip::cli
