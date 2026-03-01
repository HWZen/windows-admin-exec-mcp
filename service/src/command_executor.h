#pragma once

#include "protocol.h"

// Execute the command described by `req` and populate `resp`.
// The service process already runs with elevated (LocalSystem) privileges,
// so no elevation is needed — CreateProcess is called directly.
void execute_command(const CommandRequest& req, CommandResponse& resp);
