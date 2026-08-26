#pragma once

#include "approver.h"
#include "config.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CommandExecutor;

// Starts the TCP server on the configured address/port.
// Blocks until stop() is called from another thread.
class TcpServer {
public:
    explicit TcpServer(const ServiceConfig& config);
    ~TcpServer();

    // Start listening and serving connections.  Blocks until stop() is called.
    void run();

    // Signal the server to stop accepting new connections, terminate in-flight
    // child processes, and stop the approval backend so run() returns promptly.
    void stop();

private:
    // Join and remove client threads that have already finished.
    void reap_finished_threads();
    // Join every tracked client thread (called during shutdown).
    void join_all_threads();

    const ServiceConfig& cfg_;
    std::atomic<bool> running_{false};

    // Owned via shared_ptr so client threads can hold a reference without risk
    // of use-after-free during shutdown (BUG-H1). Atomic so the background
    // retry thread can install a late-started approver while clients connect.
    std::atomic<std::shared_ptr<Approver>> approver_;
    std::shared_ptr<CommandExecutor> executor_;

    // Retries create_approver() in the background until it succeeds or the
    // server stops (BUG-M7: a transient network failure at service start —
    // e.g. DNS not yet ready at boot — used to disable approval forever).
    void approver_retry_loop();
    std::thread approver_retry_thread_;

    // Current number of active client threads, used to enforce
    // max_concurrent_clients (ARCH-2).
    std::atomic<int> active_clients_{0};

    // All live client threads, tracked so run() can join them before the
    // approver/executor are destroyed (ARCH-7). Each entry carries a "done"
    // flag the worker sets right before it returns, letting us reap finished
    // threads without blocking on active ones.
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex threads_mtx_;
    std::vector<ClientThread> client_threads_;
};
