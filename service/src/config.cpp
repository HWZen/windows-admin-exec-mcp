#include "config.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ServiceConfig load_config(const std::string& path) {
    ServiceConfig cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        // Return defaults when no config file is present
        return cfg;
    }

    json j;
    try {
        f >> j;
    } catch (const json::parse_error&) {
        return cfg;
    }

    if (j.contains("port") && j["port"].is_number_unsigned()) {
        cfg.port = j["port"].get<uint16_t>();
    }
    if (j.contains("bind_address") && j["bind_address"].is_string()) {
        cfg.bind_address = j["bind_address"].get<std::string>();
    }
    if (j.contains("max_connections") && j["max_connections"].is_number_unsigned()) {
        cfg.max_connections = j["max_connections"].get<uint32_t>();
    }

    if (j.contains("approval") && j["approval"].is_object()) {
        auto& a = j["approval"];
        if (a.contains("enabled") && a["enabled"].is_boolean()) {
            cfg.approval.enabled = a["enabled"].get<bool>();
        }
        if (a.contains("type") && a["type"].is_string()) {
            cfg.approval.type = a["type"].get<std::string>();
        }

        if (a.contains("telegram") && a["telegram"].is_object()) {
            auto& t = a["telegram"];
            if (t.contains("bot_token") && t["bot_token"].is_string()) {
                cfg.approval.telegram.bot_token = t["bot_token"].get<std::string>();
            }
            if (t.contains("chat_id") && t["chat_id"].is_string()) {
                cfg.approval.telegram.chat_id = t["chat_id"].get<std::string>();
            }
            if (t.contains("proxy") && t["proxy"].is_string()) {
                cfg.approval.telegram.proxy = t["proxy"].get<std::string>();
            }
            if (t.contains("proxy_username") && t["proxy_username"].is_string()) {
                cfg.approval.telegram.proxy_username = t["proxy_username"].get<std::string>();
            }
            if (t.contains("proxy_password") && t["proxy_password"].is_string()) {
                cfg.approval.telegram.proxy_password = t["proxy_password"].get<std::string>();
            }
            if (t.contains("timeout_seconds") && t["timeout_seconds"].is_number_unsigned()) {
                cfg.approval.telegram.timeout_seconds = t["timeout_seconds"].get<uint32_t>();
            }
            if (t.contains("poll_interval_ms") && t["poll_interval_ms"].is_number_unsigned()) {
                cfg.approval.telegram.poll_interval_ms = t["poll_interval_ms"].get<uint32_t>();
            }
        }
    }

    return cfg;
}
