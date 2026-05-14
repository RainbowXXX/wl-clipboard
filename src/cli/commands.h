#pragma once

#include "cli/options.h"

namespace wlclip::cli {

int run_copy(const CommonOptions& opts, const std::vector<std::string>& args);
int run_paste(const CommonOptions& opts, const std::vector<std::string>& args);
int run_protocols(const CommonOptions& opts, const std::vector<std::string>& args);

}
