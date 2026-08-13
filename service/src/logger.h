#pragma once

#include <string>

// Initialize the global rotating-file logger.
// log_path — absolute path to the log file (next to the exe).
// level    — minimum log level name: "trace", "debug", "info", "warn",
//            "error", "critical", or "off". Unknown names fall back to "info".
// Creates a rotating sink: 1 MB x 3 files.
void init_logger(const std::string& log_path, const std::string& level = "info");
