#pragma once

#include <string>
#include <cstdint>

// Telegram bot configuration for approval workflow
struct TelegramConfig {
    std::string bot_token;
    std::string chat_id;
    uint32_t timeout_seconds = 300;  // Wait up to 5 minutes for approval
    uint32_t poll_interval_ms = 2000; // Poll every 2 seconds
};

// Approval gate configuration
struct ApprovalConfig {
    bool enabled = false;
    std::string type = "telegram";  // "telegram" or "none"
    TelegramConfig telegram;
};

// Top-level service configuration
struct ServiceConfig {
    uint16_t port = 12380;
    std::string bind_address = "127.0.0.1";
    uint32_t max_connections = 16;
    ApprovalConfig approval;
};

// Load configuration from a JSON file.
// Returns default config if the file does not exist or is malformed.
ServiceConfig load_config(const std::string& path);
