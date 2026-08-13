#pragma once

#include "config.h"
#include "protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// QQApprover — approval gate backed by a QQ Bot.
//
// Unlike TelegramApprover (which polls HTTP per-request), QQApprover
// maintains a persistent WebSocket connection to the QQ bot gateway so
// it can receive INTERACTION_CREATE events (button-click callbacks) in
// real time.  The WebSocket is kept alive for the lifetime of the
// TcpServer; each request_approval() call sends a group message with
// inline-keyboard Approve/Deny buttons and blocks until the matching
// callback arrives (or the timeout expires).
class QQApprover {
public:
    QQApprover();
    ~QQApprover();

    // Non-copyable / non-movable (owns threads + WinHTTP handles).
    QQApprover(const QQApprover&) = delete;
    QQApprover& operator=(const QQApprover&) = delete;

    // Connect to the QQ bot WebSocket gateway and start background
    // heartbeat / receive threads.  config_path is the path to config.json
    // so the captured user_openid can be persisted.  Returns true on success.
    bool start(const QQConfig& cfg, const std::string& config_path);

    // Signal background threads to stop, close the WebSocket, and join.
    void stop();

    // Send an approval request via QQ bot and block until the user
    // responds (or the timeout expires).  Returns true if approved.
    bool request_approval(const QQConfig& cfg, const CommandRequest& req,
                          std::string& reason);

private:
    // ---- Access-token management (libcurl HTTP) ----
    bool refresh_access_token(const QQConfig& cfg);
    std::string get_valid_token(const QQConfig& cfg);

    // ---- HTTP helpers (libcurl) ----
    static std::string http_request(const std::string& url,
                                    const std::string& method,
                                    const std::string& body,
                                    const std::string& auth);

    // ---- QQ Bot API helpers ----
    bool send_approval_message(const std::string& access_token,
                               const CommandRequest& req);
    bool send_qq_message(const std::string& access_token,
                         const std::string& content);
    void acknowledge_interaction(const std::string& interaction_id);

    // ---- WebSocket gateway (WinHTTP) ----
    bool connect_websocket(const QQConfig& cfg);
    void disconnect_websocket();
    bool ws_send_message(const std::string& msg);
    bool ws_receive_message(std::string& out);

    // ---- Background threads ----
    void heartbeat_loop();
    void receive_loop();

    // ---- Event handler ----
    void on_interaction_create(const std::string& event_body);
    void on_c2c_message_create(const std::string& event_body);

    // Returns the configured user_openid, or the auto-captured one.
    std::string get_effective_openid() const;

    // Write captured user_openid back to config.json.
    void persist_user_openid(const std::string& openid);

    // ---- Stored config (for background threads) ----
    QQConfig cfg_;
    std::string config_path_;  // For persisting captured user_openid

    // ---- Last received C2C msg_id (for passive replies) ----
    std::mutex msg_id_mtx_;
    std::string last_msg_id_;

    // ---- Access token state ----
    std::mutex token_mtx_;
    std::string access_token_;
    int64_t token_expires_at_ = 0;  // Unix seconds

    // ---- WinHTTP WebSocket handles (opaque pointers) ----
    void* hSession_  = nullptr;
    void* hConnect_  = nullptr;
    void* hWebSocket_ = nullptr;

    // ---- Threading ----
    std::thread heartbeat_thread_;
    std::thread receive_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ws_connected_{false};

    // ---- Gateway protocol state ----
    uint32_t heartbeat_interval_ms_ = 45000;
    std::atomic<long long> last_seq_{0};
    std::string session_id_;

    // ---- Approval matching ----
    // Per-request approval results keyed by request id:
    //   0 = pending, 1 = approved, 2 = denied.
    // Each concurrent request gets its own entry so a button callback can
    // only resolve its own request; no shared pending state can be
    // overwritten by a racing request.
    std::mutex approval_mtx_;
    std::condition_variable approval_cv_;
    std::unordered_map<std::string, int> pending_results_;

    // ---- Auto-captured user_openid (from first C2C message) ----
    mutable std::mutex openid_mtx_;
    std::string captured_user_openid_;
};
