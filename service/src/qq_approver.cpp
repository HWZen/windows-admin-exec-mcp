#include "qq_approver.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <spdlog/spdlog.h>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

// ===========================================================================
// Anonymous-namespace helpers
// ===========================================================================

namespace {

// ---- libcurl write callback ----
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = reinterpret_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// ---- libcurl HTTP request (GET / POST / PUT) ----
std::string http_request_impl(const std::string& url,
                              const std::string& method,
                              const std::string& body,
                              const std::string& auth,
                              bool ssl_verify) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    struct curl_slist* headers = nullptr;

    if (!auth.empty()) {
        headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    }
    if (!body.empty() || method == "POST" || method == "PUT") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, ssl_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, ssl_verify ? 2L : 0L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        spdlog::error("QQ HTTP request failed: {}", curl_easy_strerror(res));
        return {};
    }
    return response;
}

// ---- Parse a wss:// URL into host / path / port ----
struct WssUrl {
    std::string host;
    std::string path;
    uint16_t port = 443;
};

bool parse_wss_url(const std::string& url, WssUrl& out) {
    const std::string prefix = "wss://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;

    std::string rest = url.substr(prefix.size());
    size_t slash_pos = rest.find('/');
    std::string host_port =
        (slash_pos != std::string::npos) ? rest.substr(0, slash_pos) : rest;
    out.path = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "/";

    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        out.host = host_port.substr(0, colon_pos);
        out.port = static_cast<uint16_t>(std::stoi(host_port.substr(colon_pos + 1)));
    } else {
        out.host = host_port;
    }
    return !out.host.empty();
}

// ---- UTF-8 → UTF-16 ----
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

// ---- Current Unix timestamp in seconds ----
int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- Intent bit flags ----
// INTERACTION (1 << 26) — button-click callbacks
// GROUP_AND_C2C_EVENT (1 << 25) — group / C2C message events
constexpr int kIntentInteraction = 1 << 26;
constexpr int kIntentGroupAndC2C = 1 << 25;

} // anonymous namespace

// ===========================================================================
// QQApprover — public API
// ===========================================================================

QQApprover::QQApprover() = default;

QQApprover::~QQApprover() { stop(); }

bool QQApprover::start(const ServiceConfig& cfg) {
    cfg_ = cfg.approval.qq;
    config_path_ = cfg.config_path;
    ssl_verify_ = cfg.ssl_verify;
    running_ = true;

    if (!refresh_access_token()) {
        spdlog::error("QQ: failed to obtain access token");
        running_ = false;
        return false;
    }

    if (!connect_websocket()) {
        spdlog::error("QQ: failed to connect WebSocket gateway");
        running_ = false;
        return false;
    }

    heartbeat_thread_ = std::thread(&QQApprover::heartbeat_loop, this);
    receive_thread_   = std::thread(&QQApprover::receive_loop, this);

    spdlog::info("QQ: WebSocket connected, session={}", session_id_);
    return true;
}

void QQApprover::stop() {
    running_ = false;
    ws_connected_ = false;

    // Closing the WebSocket handle unblocks WinHttpWebSocketReceive in the
    // receive thread — unless that call is already wedged inside the kernel,
    // in which case nothing unblocks it and an unconditional join() hangs the
    // whole shutdown (ghost-process incident, 2026-08-25). So wait bounded:
    // if a thread does not confirm exit in time, detach it and skip handle
    // teardown; process exit reclaims everything.
    if (hWebSocket_) {
        WinHttpWebSocketShutdown(hWebSocket_,
                                 WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                 nullptr, 0);
    }

    auto wait_exit = [](std::atomic<bool>& flag, int grace_ms) {
        for (int waited = 0; waited < grace_ms; waited += 100) {
            if (flag.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return flag.load(std::memory_order_acquire);
    };

    bool recv_done = !receive_thread_.joinable() ||
                     wait_exit(receive_exited_, 10000);
    if (!recv_done) {
        spdlog::error("QQ: receive thread stuck in WinHTTP — detaching it");
        receive_thread_.detach();
    } else if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    bool hb_done = !heartbeat_thread_.joinable() ||
                   wait_exit(heartbeat_exited_, 4000);
    if (!hb_done) {
        spdlog::error("QQ: heartbeat thread stuck — detaching it");
        heartbeat_thread_.detach();
    } else if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    // Only close WinHTTP handles when no thread can still be using them.
    if (recv_done && hb_done) {
        disconnect_websocket();
    } else {
        spdlog::warn("QQ: skipping WebSocket teardown — threads detached");
    }
}

// ===========================================================================
// Access-token management
// ===========================================================================

bool QQApprover::refresh_access_token() {
    json body;
    body["appId"]        = cfg_.app_id;
    body["clientSecret"] = cfg_.app_secret;

    std::string resp = http_request_impl(
        "https://api.bot.qq.com/app/getAppAccessToken", "POST", body.dump(), "",
        ssl_verify_);

    if (resp.empty()) return false;

    try {
        auto j = json::parse(resp);
        std::string token = j.value("access_token", "");
        // expires_in may be returned as a number or a string — handle both.
        int64_t expires_in = 7200;
        if (j.contains("expires_in")) {
            if (j["expires_in"].is_string()) {
                expires_in = std::stoll(j["expires_in"].get<std::string>());
            } else if (j["expires_in"].is_number()) {
                expires_in = j["expires_in"].get<int64_t>();
            }
        }

        std::lock_guard<std::mutex> lock(token_mtx_);
        access_token_    = token;
        token_expires_at_ = now_unix_seconds() + expires_in - 60; // 60 s margin
        return !token.empty();
    } catch (...) {
        return false;
    }
}

std::string QQApprover::get_valid_token() {
    {
        std::lock_guard<std::mutex> lock(token_mtx_);
        if (!access_token_.empty() && now_unix_seconds() < token_expires_at_) {
            return access_token_;
        }
    }
    // Token is missing or about to expire — refresh outside the lock.
    if (!refresh_access_token()) return "";
    std::lock_guard<std::mutex> lock(token_mtx_);
    return access_token_;
}

// ===========================================================================
// HTTP helper (delegates to anonymous-namespace function)
// ===========================================================================

std::string QQApprover::http_request(const std::string& url,
                                     const std::string& method,
                                     const std::string& body,
                                     const std::string& auth) {
    return http_request_impl(url, method, body, auth, ssl_verify_);
}

// ===========================================================================
// QQ Bot API — send messages
// ===========================================================================

bool QQApprover::send_approval_message(const std::string& access_token,
                                       const CommandRequest& req) {
    std::string openid = get_effective_openid();
    if (openid.empty()) {
        spdlog::warn("QQ: cannot send approval — user_openid not set, send a message to the bot first");
        return false;
    }

    std::string url = "https://api.bot.qq.com/v2/users/" + openid + "/messages";

    std::ostringstream md;
    md << "## 🔐 AdminExecMCP — Approval Required\n\n";
    if (!req.description.empty()) {
      md << "**Description:** " << req.description << "\n\n";
    }
    md << "**Command:** " << req.command << "\n\n";
    if (!req.working_dir.empty()) {
        md << "**Working dir:** " << req.working_dir << "\n\n";
    }
    md << "**Timeout:** " << req.timeout_seconds << "s\n\n"
       << "**Request ID:** " << req.id << "\n\n"
       << "Click a button below to approve or deny.";

    json body;
    body["msg_type"] = 2;
    body["markdown"]["content"] = md.str();

    // Inline keyboard with Approve / Deny callback buttons.
    body["keyboard"]["content"]["rows"] = json::array({
        json::object({
            {"buttons", json::array({
                json::object({
                    {"id", "btn_approve"},
                    {"render_data", {{"label", "✅ Approve"}, {"visited_label", "✅ Approved"}, {"style", 1}}},
                    {"action", {{"type", 1},
                                {"permission", {{"type", 2}}},
                                {"data", "approve_" + req.id}}},
                }),
                json::object({
                    {"id", "btn_deny"},
                    {"render_data", {{"label", "❌ Deny"}, {"visited_label", "❌ Denied"}, {"style", 0}}},
                    {"action", {{"type", 1},
                                {"permission", {{"type", 2}}},
                                {"data", "deny_" + req.id}}},
                }),
            })},
        }),
    });

    std::string body_str = body.dump();
    spdlog::debug("QQ: send approval body: {}", body_str);
    std::string resp = http_request(url, "POST", body_str,
                                    "QQBot " + access_token);
    spdlog::info("QQ: approval POST to openid={} resp_bytes={} resp={}",
                 openid.substr(0, 8) + "...",
                 resp.size(),
                 resp.empty() ? "<empty>" : resp.substr(0, 300));
    if (resp.empty()) {
        spdlog::error("QQ: send approval failed (empty response)");
        return false;
    }
    try {
        auto j = json::parse(resp);
        if (j.contains("err_code") && j["err_code"].get<int64_t>() != 0) {
            spdlog::error("QQ: send approval failed: err_code={} message={}",
                          j["err_code"].get<int64_t>(), j.value("message", ""));
            return false;
        }
    } catch (...) {
        spdlog::error("QQ: send approval unexpected response: {}", resp.substr(0, 200));
        return false;
    }
    return true;
}

bool QQApprover::send_qq_message(const std::string& access_token,
                                 const std::string& content) {
    std::string openid = get_effective_openid();
    if (openid.empty()) return false;

    std::string url = "https://api.bot.qq.com/v2/users/" + openid + "/messages";

    json body;
    body["msg_type"] = 0;
    body["content"]  = content;

    std::string resp = http_request(url, "POST", body.dump(),
                                    "QQBot " + access_token);
    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        if (j.contains("err_code") && j["err_code"].get<int64_t>() != 0) {
            spdlog::error("QQ: send message failed: err_code={} message={}",
                          j["err_code"].get<int64_t>(), j.value("message", ""));
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

void QQApprover::acknowledge_interaction(const std::string& interaction_id) {
    std::string token = get_valid_token();
    if (token.empty()) return;

    std::string url = "https://api.bot.qq.com/interactions/" + interaction_id;
    json body;
    body["code"] = 0;

    http_request(url, "PUT", body.dump(), "QQBot " + token);
}

// ===========================================================================
// WebSocket gateway — connect / disconnect / send / receive
// ===========================================================================

bool QQApprover::connect_websocket() {
    std::string token = get_valid_token();
    if (token.empty()) return false;

    // 1. Get the WSS gateway URL.
    std::string resp = http_request("https://api.bot.qq.com/gateway/bot",
                                    "GET", "", "QQBot " + token);
    if (resp.empty()) return false;

    std::string wss_url;
    try {
        auto j = json::parse(resp);
        wss_url = j.value("url", "");
    } catch (...) {
        return false;
    }
    if (wss_url.empty()) return false;

    WssUrl parsed;
    if (!parse_wss_url(wss_url, parsed)) return false;

    // 2. Open a WinHTTP session and request a WebSocket upgrade.
    hSession_ = WinHttpOpen(L"AdminExecMCP/1.0",
                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession_) return false;

    std::wstring whost = to_wide(parsed.host);
    hConnect_ = WinHttpConnect(hSession_, whost.c_str(),
                               INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect_) {
        WinHttpCloseHandle(hSession_);
        hSession_ = nullptr;
        return false;
    }

    std::wstring wpath = to_wide(parsed.path);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect_, L"GET", wpath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect_);
        WinHttpCloseHandle(hSession_);
        hConnect_ = nullptr;
        hSession_ = nullptr;
        return false;
    }

    // Request protocol upgrade to WebSocket.
    WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                     nullptr, 0);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect_);
        WinHttpCloseHandle(hSession_);
        hConnect_ = nullptr;
        hSession_ = nullptr;
        return false;
    }

    hWebSocket_ = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest); // no longer needed after upgrade

    if (!hWebSocket_) {
        WinHttpCloseHandle(hConnect_);
        WinHttpCloseHandle(hSession_);
        hConnect_ = nullptr;
        hSession_ = nullptr;
        return false;
    }

    // 3. Wait for HELLO (op=10) which carries the heartbeat interval.
    std::string hello;
    if (!ws_receive_message(hello)) {
        disconnect_websocket();
        return false;
    }

    try {
        auto j = json::parse(hello);
        if (j.value("op", -1) != 10) {
            disconnect_websocket();
            return false;
        }
        heartbeat_interval_ms_ =
            j["d"].value("heartbeat_interval", 45000);
    } catch (...) {
        disconnect_websocket();
        return false;
    }

    // 4. Send IDENTIFY (op=2).
    json identify;
    identify["op"] = 2;
    identify["d"]["token"]    = "QQBot " + token;
    identify["d"]["intents"]  = kIntentInteraction | kIntentGroupAndC2C;
    identify["d"]["shard"]    = json::array({0, 1});

    if (!ws_send_message(identify.dump())) {
        disconnect_websocket();
        return false;
    }

    // 5. Wait for READY (op=0, t="READY").
    std::string ready;
    if (!ws_receive_message(ready)) {
        disconnect_websocket();
        return false;
    }

    try {
        auto j = json::parse(ready);
        if (j.value("op", -1) == 0 && j.value("t", "") == "READY") {
            session_id_ = j["d"].value("session_id", "");
            last_seq_   = j.value("s", 0LL);
        } else {
            disconnect_websocket();
            return false;
        }
    } catch (...) {
        disconnect_websocket();
        return false;
    }

    ws_connected_ = true;
    return true;
}

void QQApprover::disconnect_websocket() {
    if (hWebSocket_) {
        WinHttpWebSocketClose(hWebSocket_,
                              WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                              nullptr, 0);
        WinHttpCloseHandle(hWebSocket_);
        hWebSocket_ = nullptr;
    }
    if (hConnect_) {
        WinHttpCloseHandle(hConnect_);
        hConnect_ = nullptr;
    }
    if (hSession_) {
        WinHttpCloseHandle(hSession_);
        hSession_ = nullptr;
    }
}

bool QQApprover::ws_send_message(const std::string& msg) {
    if (!hWebSocket_) return false;
    DWORD result = WinHttpWebSocketSend(
        hWebSocket_,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(msg.data()),
        static_cast<DWORD>(msg.size()));
    return result == ERROR_SUCCESS;
}

bool QQApprover::ws_receive_message(std::string& out) {
    if (!hWebSocket_) return false;

    out.clear();
    char buffer[4096];
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type =
        WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;

    do {
        DWORD result = WinHttpWebSocketReceive(
            hWebSocket_, buffer, sizeof(buffer), &bytes_read, &buf_type);

        if (result != ERROR_SUCCESS) return false;

        if (buf_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;

        out.append(buffer, bytes_read);
    } while (buf_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);

    return buf_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
}

// ===========================================================================
// Background threads
// ===========================================================================

void QQApprover::heartbeat_loop() {
    while (running_ && ws_connected_) {
        // Sleep in 100 ms slices so we can exit promptly on stop().
        for (uint32_t i = 0;
             i < heartbeat_interval_ms_ / 100 && running_ && ws_connected_;
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_ || !ws_connected_) break;

        json hb;
        hb["op"] = 1;
        hb["d"]  = last_seq_.load();

        if (!ws_send_message(hb.dump())) {
            spdlog::warn("QQ: heartbeat send failed — disconnecting");
            ws_connected_ = false;
            approval_cv_.notify_all();
            break;
        }
    }
    heartbeat_exited_.store(true, std::memory_order_release);
}

void QQApprover::receive_loop() {
    while (running_) {
        if (!ws_connected_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!running_) break;

            // Join the previous heartbeat thread BEFORE closing the old handle
            // so we never close a handle that a heartbeat send() may still be
            // blocked on (BUG-M2). ws_connected_ is already false here, so the
            // old heartbeat loop is exiting; joining guarantees it is fully
            // done with the old handle before we tear it down and reconnect.
            if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

            disconnect_websocket();
            if (!connect_websocket()) {
                spdlog::warn("QQ: reconnection failed — retrying");
                continue;
            }

            heartbeat_thread_ = std::thread(&QQApprover::heartbeat_loop, this);
            spdlog::info("QQ: reconnected, session={}", session_id_);
        }

        std::string msg;
        if (!ws_receive_message(msg)) {
            ws_connected_ = false;
            approval_cv_.notify_all();
            continue;
        }

        try {
            auto j = json::parse(msg);
            int op = j.value("op", -1);

            switch (op) {
            case 0: {
                // Dispatch event
                long long s = j.value("s", 0LL);
                if (s > 0) last_seq_ = s;

                std::string t = j.value("t", "");
                spdlog::debug("QQ: event: {}", t);
                if (t == "INTERACTION_CREATE" && j.contains("d")) {
                    on_interaction_create(j["d"].dump());
                } else if (t == "C2C_MESSAGE_CREATE" && j.contains("d")) {
                    on_c2c_message_create(j["d"].dump());
                }
                break;
            }
            case 11:
                // Heartbeat ACK — nothing to do.
                break;
            case 7:
                // Server requested reconnect.
                ws_connected_ = false;
                approval_cv_.notify_all();
                break;
            case 9:
                // Invalid session — need to re-identify.
                ws_connected_ = false;
                approval_cv_.notify_all();
                break;
            default:
                break;
            }
        } catch (...) {
            // Ignore unparseable messages.
        }
    }

    ws_connected_ = false;
    approval_cv_.notify_all();
    receive_exited_.store(true, std::memory_order_release);
}

// ===========================================================================
// Event handler — INTERACTION_CREATE
// ===========================================================================

std::string QQApprover::get_effective_openid() const {
    {
        std::lock_guard<std::mutex> lock(openid_mtx_);
        if (!captured_user_openid_.empty()) return captured_user_openid_;
    }
    return cfg_.user_openid;
}

// Persist the captured user_openid back into config.json so it survives
// service restarts.
void QQApprover::persist_user_openid(const std::string& openid) {
    if (config_path_.empty()) return;
    try {
        std::ifstream fin(config_path_);
        if (!fin.is_open()) return;
        json j;
        fin >> j;
        fin.close();

        j["approval"]["qq"]["user_openid"] = openid;

        std::ofstream fout(config_path_);
        if (fout.is_open()) {
            fout << j.dump(2);
            fout.close();
            spdlog::info("QQ: persisted user_openid to config.json");
        }
    } catch (...) {
        spdlog::warn("QQ: failed to persist user_openid");
    }
}

void QQApprover::on_c2c_message_create(const std::string& event_body) {
    json d;
    try {
        d = json::parse(event_body);
    } catch (...) {
        return;
    }

    // Store the message id for passive replies.
    std::string msg_id = d.value("id", "");
    if (!msg_id.empty()) {
        std::lock_guard<std::mutex> lock(msg_id_mtx_);
        last_msg_id_ = msg_id;
    }

    // Extract user_openid.
    std::string openid;
    if (d.contains("author")) {
        openid = d["author"].value("user_openid", "");
        if (openid.empty()) {
            openid = d["author"].value("id", "");
        }
    }

    if (!openid.empty()) {
        bool was_empty = false;
        {
            std::lock_guard<std::mutex> lock(openid_mtx_);
            was_empty = captured_user_openid_.empty();
            if (was_empty) {
                captured_user_openid_ = openid;
                spdlog::info("QQ: auto-captured user_openid: {}", openid);
            }
        }
        if (was_empty) {
            persist_user_openid(openid);
            // Send a confirmation reply so the user gets visible feedback.
            std::string token = get_valid_token();
            if (!token.empty()) {
                send_qq_message(token,
                    "✅ user_openid 已捕获，审批通知已就绪。"
                    "当 AI 请求执行命令时，你会收到带按钮的审批消息。");
            }
        }
    }
}

void QQApprover::on_interaction_create(const std::string& event_body) {
    json d;
    try {
        d = json::parse(event_body);
    } catch (...) {
        return;
    }

    // Only handle message-button callbacks (type=11).
    int type = d.value("type", 0);
    if (type != 11) return;

    std::string interaction_id = d.value("id", "");
    std::string button_data;

    if (d.contains("data") && d["data"].contains("resolved")) {
        button_data = d["data"]["resolved"].value("button_data", "");
    }

    // Acknowledge the interaction so the QQ client stops showing "loading".
    if (!interaction_id.empty()) {
        acknowledge_interaction(interaction_id);
    }

    // Button data format is "approve_<req.id>" or "deny_<req.id>".
    std::string req_id;
    if (button_data.rfind("approve_", 0) == 0) {
        req_id = button_data.substr(8);
    } else if (button_data.rfind("deny_", 0) == 0) {
        req_id = button_data.substr(5);
    } else {
        return;
    }

    // Resolve only the request identified by the button data.  Each request
    // keeps its own map entry, so a callback can never touch another
    // request's pending state.
    std::lock_guard<std::mutex> lock(approval_mtx_);
    auto it = pending_results_.find(req_id);
    if (it == pending_results_.end() || it->second != 0) return;
    it->second = (button_data[0] == 'a') ? 1 : 2;  // 1=approved, 2=denied
    approval_cv_.notify_all();
}

// ===========================================================================
// request_approval — the main entry point called per command
// ===========================================================================

bool QQApprover::request_approval(const CommandRequest& req,
                                  std::string& reason) {
    spdlog::info("QQ: approval gate hit id={} command=\"{}\"",
                 req.id, req.command.substr(0, 120));
    if (!ws_connected_) {
        reason = "QQ WebSocket gateway is not connected";
        return false;
    }

    std::string token = get_valid_token();
    if (token.empty()) {
        reason = "Failed to obtain QQ access token";
        return false;
    }

    if (get_effective_openid().empty()) {
        reason = "user_openid not configured and no message received yet — "
                 "send any message to the bot first to auto-capture it";
        return false;
    }

    // Register this request's pending approval result.  Each request gets
    // its own entry keyed by req.id, so concurrent requests are independent
    // and cannot overwrite each other's matching state.
    {
        std::lock_guard<std::mutex> lock(approval_mtx_);
        pending_results_[req.id] = 0;
    }

    // Send the approval notification with Approve/Deny buttons.
    if (!send_approval_message(token, req)) {
        {
            std::lock_guard<std::mutex> lock(approval_mtx_);
            pending_results_.erase(req.id);
        }
        reason = "Failed to send QQ approval notification";
        return false;
    }

    // Block until this request's matching callback arrives or the timeout
    // expires.  wait_for temporarily releases the lock while waiting,
    // allowing on_interaction_create to resolve this request's entry.
    bool got_response = false;
    {
        std::unique_lock<std::mutex> lock(approval_mtx_);
        got_response = approval_cv_.wait_for(
            lock, std::chrono::seconds(cfg_.timeout_seconds),
            [this, &req] {
                auto it = pending_results_.find(req.id);
                return (it != pending_results_.end() && it->second != 0) ||
                       !ws_connected_;
            });
    }

    if (!ws_connected_) {
        {
            std::lock_guard<std::mutex> lock(approval_mtx_);
            pending_results_.erase(req.id);
        }
        reason = "QQ WebSocket disconnected while waiting for approval";
        return false;
    }

    int result = 0;
    {
        std::lock_guard<std::mutex> lock(approval_mtx_);
        auto it = pending_results_.find(req.id);
        if (it != pending_results_.end()) {
            result = it->second;
            pending_results_.erase(it);
        }
    }

    if (!got_response || result == 0) {
        spdlog::info("QQ: approval id={} timed out after {}s",
                     req.id, cfg_.timeout_seconds);
        reason = "Approval timed out after " +
                 std::to_string(cfg_.timeout_seconds) + " seconds";
        send_qq_message(token, "⏰ Timed out: " + req.id);
        return false;
    }

    if (result == 1) {
        spdlog::info("QQ: approval id={} APPROVED via button", req.id);
        send_qq_message(token, "✅ Approved: " + req.id);
        return true;
    }

    spdlog::info("QQ: approval id={} DENIED via button", req.id);
    reason = "Request denied via QQ";
    send_qq_message(token, "❌ Denied: " + req.id);
    return false;
}
