#pragma once

#include <string>

// Initialize the global rotating-file logger.
// log_path — absolute path to the log file (next to the exe).
// Creates a rotating sink: 1 MB x 3 files.
void init_logger(const std::string& log_path);
