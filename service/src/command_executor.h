#pragma once

#include "protocol.h"

#include <mutex>
#include <vector>

// Executes commands via cmd.exe /c using the service's (LocalSystem) token.
//
// Also tracks the handles of in-flight child processes so the service can
// terminate them during graceful shutdown (ARCH-7), preventing orphaned
// cmd.exe processes from outliving the service.
//
// Child process handles are stored as void* (HANDLE is a void*) to avoid
// pulling <windows.h> into this header and disrupting the required
// winsock2.h-before-windows.h include order in tcp_server.cpp.
class CommandExecutor {
public:
    // Execute the command described by `req` and populate `resp`.
    void execute(const CommandRequest& req, CommandResponse& resp);

    // Terminate every in-flight child process. Called on shutdown.
    void terminate_all();

private:
    std::mutex mtx_;
    std::vector<void*> active_processes_;  // raw child process HANDLEs
};
