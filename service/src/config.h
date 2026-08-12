#pragma once

#include <string>
#include <cstdint>

// Telegram bot configuration for approval workflow
struct TelegramConfig {
    std::string bot_token;
    std::string chat_id;
    std::string proxy;
    std::string proxy_username;
    std::string proxy_password;
    uint32_t timeout_seconds = 300;  // Wait up to 5 minutes for approval
    uint32_t poll_interval_ms = 2000; // Poll every 2 seconds
};

// QQ bot configuration for approval workflow
struct QQConfig {
    std::string app_id;
    std::string app_secret;
    std::string user_openid;       // C2C (single-chat) user OpenID.
                                   // Leave empty to auto-capture from the
                                   // first message the user sends to the bot.
    uint32_t timeout_seconds = 300;  // Wait up to 5 minutes for approval
};

// Approval gate configuration
struct ApprovalConfig {
    bool enabled = false;
    std::string type = "telegram";  // "telegram", "qq", or "none"
    TelegramConfig telegram;
    QQConfig qq;
};

// Top-level service configuration
struct ServiceConfig {
    uint16_t port = 12380;
    std::string bind_address = "127.0.0.1";
    uint32_t max_connections = 16;
    ApprovalConfig approval;
    std::string config_path;  // Path to config.json (set by main, not parsed from JSON)
};

// Load configuration from a JSON file.
// Returns default config if the file does not exist or is malformed.
ServiceConfig load_config(const std::string& path);
