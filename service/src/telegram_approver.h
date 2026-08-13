#pragma once

#include "approver.h"
#include "config.h"
#include "protocol.h"

#include <atomic>

// Approval gate backed by the Telegram Bot API (stateless HTTP long-polling).
// Implements the unified Approver interface.
class TelegramApprover : public Approver {
public:
    // Ask the configured approval backend whether `req` may proceed.
    bool request_approval(const CommandRequest& req, std::string& reason) override;

    bool start(const ServiceConfig& cfg) override;
    void stop() override;

private:
    long long get_offset_snapshot() const;
    void advance_offset_if_needed(long long new_offset);

    TelegramConfig cfg_;
    bool ssl_verify_ = true;          // SEC-C2: copied from ServiceConfig
    std::atomic<bool> running_{false};
    // BUG-M1: offset_ is read/written from concurrent client threads, so it
    // must be an atomic to avoid a data race.
    std::atomic<long long> offset_{0};
};
