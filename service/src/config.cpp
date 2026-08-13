#include "config.h"

#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace {

// Read a string field, falling back to `fallback` when missing or wrong-typed.
std::string get_string(const json& j, const char* key, std::string fallback) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    return j[key].get<std::string>();
}

// Read a boolean field, falling back to `fallback` when missing or wrong-typed.
bool get_bool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key) || !j[key].is_boolean()) return fallback;
    return j[key].get<bool>();
}

// Read an unsigned integer field with an explicit range check.
//
// NOTE (BUG-C1): nlohmann::json::get<uint16_t>() does NOT throw out_of_range
// for an out-of-range value — it silently static_casts (truncates). E.g.
// `json(99999).get<uint16_t>()` yields 34463. To actually reject a bad port
// instead of binding to a garbage port, the range must be checked explicitly
// here before narrowing.
template <typename T>
T get_uint(const json& j, const char* key, T fallback) {
    if (!j.contains(key) || !j[key].is_number_unsigned()) return fallback;
    uint64_t v = j[key].get<uint64_t>();
    if (v > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
        spdlog::warn("config: '{}' value {} out of range, using default {}",
                     key, v, static_cast<uint64_t>(fallback));
        return fallback;
    }
    return static_cast<T>(v);
}

} // anonymous namespace

ServiceConfig load_config(const std::string& path) {
    ServiceConfig cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        // Return defaults when no config file is present.
        return cfg;
    }

    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        spdlog::warn("config: failed to parse {}: {}", path, e.what());
        return cfg;
    }

    cfg.port         = get_uint<uint16_t>(j, "port", cfg.port);
    cfg.bind_address = get_string(j, "bind_address", cfg.bind_address);

    // listen_backlog is the current name; max_connections is kept as a
    // deprecated alias so existing config.json files keep working (ARCH-2).
    if (j.contains("listen_backlog")) {
        cfg.listen_backlog = get_uint<uint32_t>(j, "listen_backlog", cfg.listen_backlog);
    } else if (j.contains("max_connections")) {
        cfg.listen_backlog = get_uint<uint32_t>(j, "max_connections", cfg.listen_backlog);
        spdlog::warn("config: 'max_connections' is deprecated, use 'listen_backlog'");
    }
    cfg.max_concurrent_clients =
        get_uint<uint32_t>(j, "max_concurrent_clients", cfg.max_concurrent_clients);

    cfg.ssl_verify = get_bool(j, "ssl_verify", cfg.ssl_verify);
    cfg.log_level  = get_string(j, "log_level", cfg.log_level);

    if (j.contains("approval") && j["approval"].is_object()) {
        const auto& a = j["approval"];
        cfg.approval.enabled = get_bool(a, "enabled", cfg.approval.enabled);
        cfg.approval.type    = get_string(a, "type", cfg.approval.type);

        if (a.contains("telegram") && a["telegram"].is_object()) {
            const auto& t = a["telegram"];
            cfg.approval.telegram.bot_token       = get_string(t, "bot_token", cfg.approval.telegram.bot_token);
            cfg.approval.telegram.chat_id         = get_string(t, "chat_id", cfg.approval.telegram.chat_id);
            cfg.approval.telegram.proxy           = get_string(t, "proxy", cfg.approval.telegram.proxy);
            cfg.approval.telegram.proxy_username  = get_string(t, "proxy_username", cfg.approval.telegram.proxy_username);
            cfg.approval.telegram.proxy_password  = get_string(t, "proxy_password", cfg.approval.telegram.proxy_password);
            cfg.approval.telegram.timeout_seconds = get_uint<uint32_t>(t, "timeout_seconds", cfg.approval.telegram.timeout_seconds);
            cfg.approval.telegram.poll_interval_ms = get_uint<uint32_t>(t, "poll_interval_ms", cfg.approval.telegram.poll_interval_ms);
        }

        if (a.contains("qq") && a["qq"].is_object()) {
            const auto& q = a["qq"];
            cfg.approval.qq.app_id         = get_string(q, "app_id", cfg.approval.qq.app_id);
            cfg.approval.qq.app_secret     = get_string(q, "app_secret", cfg.approval.qq.app_secret);
            cfg.approval.qq.user_openid    = get_string(q, "user_openid", cfg.approval.qq.user_openid);
            cfg.approval.qq.timeout_seconds = get_uint<uint32_t>(q, "timeout_seconds", cfg.approval.qq.timeout_seconds);
        }
    }

    return cfg;
}
