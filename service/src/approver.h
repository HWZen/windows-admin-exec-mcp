#pragma once

#include "config.h"
#include "protocol.h"

#include <string>

// Unified abstraction for command-approval backends (Telegram, QQ, or future).
//
// TelegramApprover (stateless HTTP long-polling) and QQApprover (stateful
// WebSocket) both implement this interface so the TcpServer only ever depends
// on the abstract type. Adding a third approval backend therefore does not
// require touching any connection-handling code (ARCH-1).
class Approver {
public:
    virtual ~Approver() = default;

    // Ask the approval backend whether `req` may proceed.
    // Returns true if approved, false if denied or timed out.
    // `reason` is set to a human-readable explanation on failure.
    virtual bool request_approval(const CommandRequest& req,
                                  std::string& reason) = 0;

    // Lifecycle hooks. Stateless backends may rely on the default no-op
    // implementations; stateful backends override them to acquire/release
    // their persistent resources.
    virtual bool start(const ServiceConfig& cfg) { (void)cfg; return true; }
    virtual void stop() {}
};
