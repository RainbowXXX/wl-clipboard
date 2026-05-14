#include "core/log.h"

#include <spdlog/sinks/stderr_color_sink.h>

#include <memory>

namespace wlclip::core {

void init_logging(spdlog::level::level_enum level) {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("wlclip", sink);
    logger->set_level(level);
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::warn);
}

}
