#include "telegram_approver.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <thread>
#include <string>

using json = nlohmann::json;

namespace {

// libcurl write callback — appends received data to a std::string.
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = reinterpret_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Perform a GET/POST request and return the response body.
// Returns empty string on error.
std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return response;
}

std::string http_post_json(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return response;
}

// Send a Telegram message. Returns true on success.
bool send_telegram_message(const std::string& token,
                           const std::string& chat_id,
                           const std::string& text) {
    std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
    json body;
    body["chat_id"]    = chat_id;
    body["text"]       = text;
    body["parse_mode"] = "HTML";
    std::string resp   = http_post_json(url, body.dump());
    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        return j.value("ok", false);
    } catch (...) {
        return false;
    }
}

// Poll for updates with offset, return new updates.
json get_updates(const std::string& token, long long offset) {
    std::ostringstream url;
    url << "https://api.telegram.org/bot" << token
        << "/getUpdates?timeout=5&offset=" << offset;
    std::string resp = http_get(url.str());
    if (resp.empty()) return {};
    try {
        return json::parse(resp);
    } catch (...) {
        return {};
    }
}

} // anonymous namespace

bool request_approval(const TelegramConfig& cfg,
                      const CommandRequest& req,
                      std::string& reason) {
    // Compose notification message (HTML parse mode)
    std::ostringstream msg;
    msg << "🔐 <b>AdminExecMCP — Approval Required</b>\n\n"
        << "Command:\n<pre>" << req.command << "</pre>\n";
    if (!req.working_dir.empty()) {
        msg << "Working dir: <code>" << req.working_dir << "</code>\n";
    }
    msg << "Timeout: " << req.timeout_seconds << "s\n"
        << "Request ID: <code>" << req.id << "</code>\n\n"
        << "Reply:\n"
        << "  /approve_" << req.id << "  — to approve\n"
        << "  /deny_"    << req.id << "  — to deny";

    if (!send_telegram_message(cfg.bot_token, cfg.chat_id, msg.str())) {
        reason = "Failed to send Telegram notification";
        return false;
    }

    // Poll for /approve_<id> or /deny_<id>
    long long offset = 0;
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(cfg.timeout_seconds);

    std::string approve_cmd = "/approve_" + req.id;
    std::string deny_cmd    = "/deny_"    + req.id;

    while (std::chrono::steady_clock::now() < deadline) {
        json updates = get_updates(cfg.bot_token, offset);
        if (updates.contains("result") && updates["result"].is_array()) {
            for (auto& upd : updates["result"]) {
                long long upd_id = upd.value("update_id", 0LL);
                if (upd_id >= offset) offset = upd_id + 1;

                std::string text;
                if (upd.contains("message") &&
                    upd["message"].contains("text") &&
                    upd["message"]["text"].is_string()) {
                    text = upd["message"]["text"].get<std::string>();
                }

                if (text == approve_cmd) {
                    send_telegram_message(cfg.bot_token, cfg.chat_id,
                        "✅ Approved: <code>" + req.id + "</code>");
                    return true;
                }
                if (text == deny_cmd) {
                    send_telegram_message(cfg.bot_token, cfg.chat_id,
                        "❌ Denied: <code>" + req.id + "</code>");
                    reason = "Request denied via Telegram";
                    return false;
                }
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg.poll_interval_ms));
    }

    reason = "Approval timed out after " + std::to_string(cfg.timeout_seconds) + " seconds";
    send_telegram_message(cfg.bot_token, cfg.chat_id,
        "⏰ Timed out: <code>" + req.id + "</code>");
    return false;
}
