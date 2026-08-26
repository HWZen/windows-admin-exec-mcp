#include "logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

void init_logger(const std::string& log_path, const std::string& level) {
    // Rotating file sink: max 1 MB per file, keep 3 rotated files.
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path, 1024 * 1024, 3);

    auto logger = std::make_shared<spdlog::logger>("admexec", sink);

    // Parse the configured level; fall back to info for unrecognized names.
    auto lvl = spdlog::level::from_str(level);
    if (lvl == spdlog::level::off && level != "off") {
        lvl = spdlog::level::info;
    }
    logger->set_level(lvl);

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    // Flush everything immediately: this service logs very little, and
    // buffered debug lines are worthless during incident diagnosis.
    logger->flush_on(spdlog::level::debug);
    spdlog::set_default_logger(logger);
}
