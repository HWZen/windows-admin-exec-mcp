#pragma once

#include <string>
#include <cstdint>

// JSON over TCP length-prefixed framing:
//   [4 bytes big-endian length][JSON payload bytes]

// Request sent from the MCP client to this service.
struct CommandRequest {
    std::string id;               // Unique request UUID
    std::string command;          // Command line to execute (passed to cmd.exe /c)
    std::string description;      // AI-provided human-readable explanation shown in approval messages
    std::string working_dir;      // Working directory (empty = inherit service process working directory)
    uint32_t timeout_seconds = 60; // Execution timeout
};

// Response returned from this service to the MCP client.
struct CommandResponse {
    std::string id;               // Mirrors the request id
    bool success = false;
    std::string stdout_output;
    std::string stderr_output;
    int exit_code = -1;
    std::string error_message;   // Non-empty when success == false
};
