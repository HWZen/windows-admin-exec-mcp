#pragma once

#include "config.h"
#include "protocol.h"
#include <mutex>

class TelegramApprover {
public:
  // Ask the configured approval backend whether `req` may proceed.
  // Returns true if approved, false if denied or timed out.
  // `reason` is set to a human-readable explanation on failure.
  bool request_approval(const TelegramConfig &cfg, const CommandRequest &req,
                        std::string &reason);

private:
  long long get_offset_snapshot();
  void advance_offset_if_needed(long long new_offset);

  long long offset_{0};
};


