#include "logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

void init_logger(const std::string& log_path) {
    // Rotating file sink: max 1 MB per file, keep 3 rotated files.
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path, 1024 * 1024, 3);

    auto logger = std::make_shared<spdlog::logger>("admexec", sink);
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    logger->flush_on(spdlog::level::debug);  // flush on every message
    spdlog::set_default_logger(logger);
}
