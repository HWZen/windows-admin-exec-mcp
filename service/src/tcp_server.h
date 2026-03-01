#pragma once

#include "config.h"
#include <atomic>
#include <string>
#include <cstdint>

// Starts the TCP server on the configured address/port.
// Blocks until stop() is called from another thread.
class TcpServer {
public:
    explicit TcpServer(const ServiceConfig& config);
    ~TcpServer();

    // Start listening and serving connections.  Blocks until stop() is called.
    void run();

    // Signal the server to stop accepting new connections and exit run().
    void stop();

private:
    const ServiceConfig& cfg_;
    std::atomic<bool> running_{false};
};
