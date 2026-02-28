#pragma once

#include "config.h"
#include "protocol.h"

// Ask the configured approval backend whether `req` may proceed.
// Returns true if approved, false if denied or timed out.
// `reason` is set to a human-readable explanation on failure.
bool request_approval(const TelegramConfig& cfg,
                      const CommandRequest& req,
                      std::string& reason);
