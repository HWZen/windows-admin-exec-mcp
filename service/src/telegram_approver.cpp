#include "telegram_approver.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <string>
#include <iostream>

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
std::string http_get(const std::string& url,
                     const std::string& proxy,
                     const std::string& proxy_username,
                     const std::string& proxy_password) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    if (!proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    }
    if (!proxy_username.empty()) {
        std::string proxy_userpwd = proxy_username + ":" + proxy_password;
        curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxy_userpwd.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "HTTP request failed: " << curl_easy_strerror(res) << std::endl;
        return {};
    }
    return response;
}

std::string http_post_json(const std::string& url,
                           const std::string& body,
                           const std::string& proxy,
                           const std::string& proxy_username,
                           const std::string& proxy_password) {
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    if (!proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    }
    if (!proxy_username.empty()) {
        std::string proxy_userpwd = proxy_username + ":" + proxy_password;
        curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, proxy_userpwd.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "HTTP request failed: " << curl_easy_strerror(res) << std::endl;
        return {};
    }
    return response;
}

// Escape HTML entities in a string so it is safe to embed in parse_mode=HTML.
std::string html_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '&':  result += "&amp;";  break;
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&#39;";  break;
            default:   result += static_cast<char>(c);
        }
    }
    return result;
}

// Send a Telegram message with an inline keyboard containing Approve/Deny
// buttons.  Returns the message_id on success, or -1 on failure.
int send_approval_message(const std::string& token,
                          const std::string& chat_id,
                          const std::string& text,
                          const std::string& approve_data,
                          const std::string& deny_data,
                          const std::string& proxy,
                          const std::string& proxy_username,
                          const std::string& proxy_password) {
    std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
    json body;
    body["chat_id"]    = chat_id;
    body["text"]       = text;
    body["parse_mode"] = "HTML";
    body["reply_markup"]["inline_keyboard"] = json::array({
        json::array({
            json::object({{"text", "✅ Approve"}, {"callback_data", approve_data}}),
            json::object({{"text", "❌ Deny"},    {"callback_data", deny_data}})
        })
    });

    std::string resp = http_post_json(url, body.dump(), proxy, proxy_username, proxy_password);
    if (resp.empty()) return -1;
    try {
        auto j = json::parse(resp);
        if (!j.value("ok", false)) return -1;
        return j["result"].value("message_id", -1);
    } catch (...) {
        return -1;
    }
}

// Send a plain Telegram message (no keyboard). Returns true on success.
bool send_telegram_message(const std::string& token,
                           const std::string& chat_id,
                           const std::string& text,
                           const std::string& proxy,
                           const std::string& proxy_username,
                           const std::string& proxy_password) {
    std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
    json body;
    body["chat_id"]    = chat_id;
    body["text"]       = text;
    body["parse_mode"] = "HTML";
    std::string resp   = http_post_json(url, body.dump(), proxy, proxy_username, proxy_password);
    if (resp.empty()) return false;
    try {
        auto j = json::parse(resp);
        return j.value("ok", false);
    } catch (...) {
        return false;
    }
}

// Acknowledge a callback query so Telegram removes the "loading" indicator.
void answer_callback_query(const std::string& token,
                           const std::string& cq_id,
                           const std::string& proxy,
                           const std::string& proxy_username,
                           const std::string& proxy_password) {
    std::string url = "https://api.telegram.org/bot" + token + "/answerCallbackQuery";
    json body;
    body["callback_query_id"] = cq_id;
    http_post_json(url, body.dump(), proxy, proxy_username, proxy_password);
}

// Poll for updates with offset, return new updates.
json get_updates(const std::string& token,
                 long long offset,
                 const std::string& proxy,
                 const std::string& proxy_username,
                 const std::string& proxy_password) {
    std::ostringstream url;
    url << "https://api.telegram.org/bot" << token
        << "/getUpdates?timeout=5&offset=" << offset;
    std::string resp = http_get(url.str(), proxy, proxy_username, proxy_password);
    if (resp.empty()) return {};
    try {
        return json::parse(resp);
    } catch (...) {
        return {};
    }
}

} // anonymous namespace

long long TelegramApprover::get_offset_snapshot() {
    return offset_;
}

void TelegramApprover::advance_offset_if_needed(long long new_offset) {
    if (new_offset > offset_) {
        offset_ = new_offset;
    }
}

bool TelegramApprover::request_approval(const TelegramConfig &cfg,
                                      const CommandRequest &req,
                                      std::string &reason) {
  // Unique callback_data tokens for this request's buttons.
  const std::string approve_data = "approve_" + req.id;
  const std::string deny_data = "deny_" + req.id;

  // Compose notification message (HTML parse mode).
  // User-controlled fields (command, working_dir) are HTML-escaped to
  // prevent markup injection.
  std::ostringstream msg;
  msg << "🔐 <b>AdminExecMCP — Approval Required</b>\n\n"
      << "Command:\n<pre>" << html_escape(req.command) << "</pre>\n";
  if (!req.working_dir.empty()) {
    msg << "Working dir: <code>" << html_escape(req.working_dir) << "</code>\n";
  }
  msg << "Timeout: " << req.timeout_seconds << "s\n"
      << "Request ID: <code>" << req.id << "</code>\n\n"
      << "Use the buttons below to approve or deny.";

  if (send_approval_message(cfg.bot_token, cfg.chat_id, msg.str(), approve_data,
                            deny_data, cfg.proxy, cfg.proxy_username,
                            cfg.proxy_password) < 0) {
    reason = "Failed to send Telegram notification";
    return false;
  }

  // Retrieve the persisted offset so we only process new updates.
  long long offset = get_offset_snapshot();

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.timeout_seconds);

  while (std::chrono::steady_clock::now() < deadline) {
    json updates = get_updates(cfg.bot_token, offset, cfg.proxy,
                               cfg.proxy_username, cfg.proxy_password);
    if (updates.contains("result") && updates["result"].is_array()) {
      for (auto &upd : updates["result"]) {
        long long upd_id = upd.value("update_id", 0LL);
        if (upd_id >= offset) {
          long long new_off = upd_id + 1;
          advance_offset_if_needed(new_off);
          offset = new_off;
        }

        // Handle inline-keyboard callback queries.
        if (upd.contains("callback_query")) {
          auto &cq = upd["callback_query"];

          // Validate that the response comes from the configured chat.
          long long from_chat = 0;
          if (cq.contains("message") && cq["message"].contains("chat")) {
            from_chat = cq["message"]["chat"].value("id", 0LL);
          }
          if (std::to_string(from_chat) != cfg.chat_id)
            continue;

          std::string cq_id = cq.value("id", "");
          std::string cb_data = cq.value("data", "");

          if (cb_data == approve_data) {
            answer_callback_query(cfg.bot_token, cq_id, cfg.proxy,
                                  cfg.proxy_username, cfg.proxy_password);
            send_telegram_message(cfg.bot_token, cfg.chat_id,
                                  "✅ Approved: <code>" + req.id + "</code>",
                                  cfg.proxy, cfg.proxy_username,
                                  cfg.proxy_password);
            return true;
          }
          if (cb_data == deny_data) {
            answer_callback_query(cfg.bot_token, cq_id, cfg.proxy,
                                  cfg.proxy_username, cfg.proxy_password);
            send_telegram_message(cfg.bot_token, cfg.chat_id,
                                  "❌ Denied: <code>" + req.id + "</code>",
                                  cfg.proxy, cfg.proxy_username,
                                  cfg.proxy_password);
            reason = "Request denied via Telegram";
            return false;
          }
        }
      }
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(cfg.poll_interval_ms));
  }

  reason = "Approval timed out after " + std::to_string(cfg.timeout_seconds) +
           " seconds";
  send_telegram_message(cfg.bot_token, cfg.chat_id,
                        "⏰ Timed out: <code>" + req.id + "</code>", cfg.proxy,
                        cfg.proxy_username, cfg.proxy_password);
  return false;
}
