#include "tcp_server.h"
#include "command_executor.h"
#include "telegram_approver.h"
#include "protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <thread>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Framing helpers: 4-byte big-endian length prefix + JSON body
// ---------------------------------------------------------------------------

namespace {

bool send_all(SOCKET s, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r == SOCKET_ERROR) return false;
        sent += r;
    }
    return true;
}

bool recv_all(SOCKET s, char* buf, int len) {
    int received = 0;
    while (received < len) {
        int r = recv(s, buf + received, len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

bool send_message(SOCKET s, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    if (!send_all(s, reinterpret_cast<const char*>(&len), 4)) return false;
    return send_all(s, msg.data(), static_cast<int>(msg.size()));
}

bool recv_message(SOCKET s, std::string& msg) {
    uint32_t len_net = 0;
    if (!recv_all(s, reinterpret_cast<char*>(&len_net), 4)) return false;
    uint32_t len = ntohl(len_net);
    if (len == 0 || len > 16 * 1024 * 1024) return false; // 16 MB sanity limit
    msg.resize(len);
    return recv_all(s, msg.data(), static_cast<int>(len));
}

// Parse CommandRequest from JSON
bool parse_request(const std::string& raw, CommandRequest& req) {
    try {
        auto j = json::parse(raw);
        req.id             = j.value("id", "");
        req.command        = j.value("command", "");
        req.working_dir    = j.value("working_dir", "");
        req.timeout_seconds = j.value("timeout_seconds", 60u);
        return !req.id.empty() && !req.command.empty();
    } catch (...) {
        return false;
    }
}

// Serialize CommandResponse to JSON
std::string serialize_response(const CommandResponse& resp) {
    json j;
    j["id"]            = resp.id;
    j["success"]       = resp.success;
    j["stdout_output"] = resp.stdout_output;
    j["stderr_output"] = resp.stderr_output;
    j["exit_code"]     = resp.exit_code;
    j["error_message"] = resp.error_message;
    return j.dump();
}

// Handle a single client connection in its own thread.
void handle_client(SOCKET client_sock, const ServiceConfig& cfg) {
    // Set a receive timeout so a stalled client won't hold a thread forever
    DWORD timeout_ms = 30 * 1000; // 30 seconds for receiving a request
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    std::string raw;
    if (!recv_message(client_sock, raw)) {
        closesocket(client_sock);
        return;
    }

    CommandRequest req;
    CommandResponse resp;

    if (!parse_request(raw, req)) {
        resp.success = false;
        resp.error_message = "Invalid request format";
        send_message(client_sock, serialize_response(resp));
        closesocket(client_sock);
        return;
    }

    resp.id = req.id;

    // Approval gate (optional)
    if (cfg.approval.enabled && cfg.approval.type == "telegram") {
        std::string reason;
        if (!request_approval(cfg.approval.telegram, req, reason)) {
            resp.success       = false;
            resp.error_message = "Approval denied: " + reason;
            send_message(client_sock, serialize_response(resp));
            closesocket(client_sock);
            return;
        }
    }

    execute_command(req, resp);
    send_message(client_sock, serialize_response(resp));
    closesocket(client_sock);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TcpServer implementation
// ---------------------------------------------------------------------------

TcpServer::TcpServer(const ServiceConfig& config) : cfg_(config) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

TcpServer::~TcpServer() {
    WSACleanup();
}

void TcpServer::run() {
    running_ = true;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        throw std::runtime_error("socket() failed");
    }

    // Allow quick restart
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        throw std::runtime_error("bind() failed on port " + std::to_string(cfg_.port));
    }

    if (listen(listen_sock, static_cast<int>(cfg_.max_connections)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        throw std::runtime_error("listen() failed");
    }

    // Set a select timeout so stop() takes effect promptly
    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock, &rfds);

        timeval tv{1, 0}; // 1-second timeout
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        SOCKET client = accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        // Spawn a detached thread per connection
        std::thread([client, this]() {
            handle_client(client, cfg_);
        }).detach();
    }

    closesocket(listen_sock);
}

void TcpServer::stop() {
    running_ = false;
}
