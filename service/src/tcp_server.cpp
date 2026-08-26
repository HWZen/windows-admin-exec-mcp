#include "tcp_server.h"
#include "command_executor.h"
#include "telegram_approver.h"
#include "qq_approver.h"
#include "protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <string>
#include <stdexcept>
#include <memory>
#include <cstring>
#include <cstddef>

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

// Maximum response size — mirrors the 16 MB inbound limit so the Python
// client can always receive the framed message.
static constexpr size_t kMaxResponseBytes = 16ULL * 1024 * 1024;
// Reserve 1 MB inside the limit for JSON overhead and the error message.
static constexpr size_t kOutputBudget = kMaxResponseBytes - 1024 * 1024;

// Serialize CommandResponse to JSON.
std::string serialize_response(const CommandResponse& resp) {
    json j;
    j["id"]            = resp.id;
    j["success"]       = resp.success;
    j["stdout_output"] = resp.stdout_output;
    j["stderr_output"] = resp.stderr_output;
    j["exit_code"]     = resp.exit_code;
    j["error_message"] = resp.error_message;

    std::string result = j.dump(-1, ' ', false, json::error_handler_t::replace);

    if (result.size() <= kMaxResponseBytes) {
        return result;
    }

    size_t out_len = resp.stdout_output.size() + resp.stderr_output.size();
    if (out_len > 0) {
        double factor = static_cast<double>(kOutputBudget) / static_cast<double>(out_len) * 0.9;
        if (factor > 1.0) factor = 1.0;

        size_t keep_stdout = static_cast<size_t>(resp.stdout_output.size() * factor);
        size_t keep_stderr = static_cast<size_t>(resp.stderr_output.size() * factor);

        j["stdout_output"] = resp.stdout_output.substr(0, keep_stdout);
        j["stderr_output"] = resp.stderr_output.substr(0, keep_stderr);
    }

    std::string trunc_msg = "[Output truncated: response exceeded 16 MB limit]";
    j["error_message"] = resp.error_message.empty()
        ? trunc_msg
        : resp.error_message + " | " + trunc_msg;

    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Handle a single client connection in its own thread.
void handle_client(SOCKET client_sock,
                   const std::shared_ptr<Approver>& approver,
                   const std::shared_ptr<CommandExecutor>& executor) {
    // Set receive/send timeouts so a stalled client won't hold a thread
    // forever (BUG-L3: send_all previously had no timeout and could block
    // indefinitely if the client stopped reading the response).
    DWORD timeout_ms = 30 * 1000; // 30 seconds
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO,
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

    // Everything from here is in the context of this client's request.
    // Catch any exception, report it back to the caller instead of crashing.
    try {
        // Approval gate (optional) — via the unified Approver interface.
        if (approver) {
            std::string reason;
            if (!approver->request_approval(req, reason)) {
                resp.success       = false;
                resp.error_message = "Approval denied: " + reason;
                send_message(client_sock, serialize_response(resp));
                closesocket(client_sock);
                return;
            }
        }

        executor->execute(req, resp);
        send_message(client_sock, serialize_response(resp));
    } catch (const std::exception& e) {
        resp.success = false;
        resp.error_message = std::string("Internal server error: ") + e.what();
        send_message(client_sock, serialize_response(resp));
    } catch (...) {
        resp.success = false;
        resp.error_message = "Internal server error: unknown exception";
        send_message(client_sock, serialize_response(resp));
    }

    closesocket(client_sock);
}

// Instantiate the configured approval backend, or return nullptr if approval
// is disabled or could not be started.
std::shared_ptr<Approver> create_approver(const ServiceConfig& cfg) {
    if (!cfg.approval.enabled) return nullptr;

    std::shared_ptr<Approver> approver;
    if (cfg.approval.type == "telegram") {
        approver = std::make_shared<TelegramApprover>();
    } else if (cfg.approval.type == "qq") {
        approver = std::make_shared<QQApprover>();
    } else {
        spdlog::warn("unknown approval type '{}' — approval disabled", cfg.approval.type);
        return nullptr;
    }

    if (!approver->start(cfg)) {
        spdlog::error("Failed to start '{}' approver — approval will be unavailable",
                      cfg.approval.type);
        approver.reset();
    }
    return approver;
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

    // Exclusive bind (SEC/OPS): with SO_REUSEADDR a hung previous instance
    // that has not released its listening socket lets this process bind the
    // same port successfully, and new connections then land in an accept
    // backlog nobody serves ("ghost" listener, observed 2026-08-25). Failing
    // loudly here beats silently sharing the port.
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    int pton_result = inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr);
    if (pton_result != 1) {
        closesocket(listen_sock);
        throw std::runtime_error("invalid bind address: " + cfg_.bind_address);
    }

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        throw std::runtime_error("bind() failed on port " + std::to_string(cfg_.port));
    }

    // listen_backlog (renamed from max_connections) is only the accept backlog.
    if (listen(listen_sock, static_cast<int>(cfg_.listen_backlog)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        throw std::runtime_error("listen() failed");
    }

    approver_ = create_approver(cfg_);
    if (cfg_.approval.enabled && !approver_.load()) {
        spdlog::warn("approval backend unavailable at startup — retrying in background");
        approver_retry_thread_ = std::thread(&TcpServer::approver_retry_loop, this);
    }
    executor_ = std::make_shared<CommandExecutor>();

    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock, &rfds);

        timeval tv{1, 0}; // 1-second timeout so stop() takes effect promptly
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            reap_finished_threads();
            continue;
        }

        SOCKET client = accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            reap_finished_threads();
            continue;
        }

        // Enforce the real concurrent-client limit (ARCH-2). Reject the
        // connection once the limit is reached.
        if (cfg_.max_concurrent_clients > 0) {
            int prev = active_clients_.fetch_add(1);
            if (prev >= static_cast<int>(cfg_.max_concurrent_clients)) {
                active_clients_.fetch_sub(1);
                closesocket(client);
                spdlog::warn("rejecting connection: max_concurrent_clients ({}) reached",
                             cfg_.max_concurrent_clients);
                continue;
            }
        } else {
            active_clients_.fetch_add(1);
        }

        auto done = std::make_shared<std::atomic<bool>>(false);
        // Copy the shared_ptrs into the worker so the approver/executor stay
        // alive for as long as any client thread is using them (BUG-H1).
        auto approver = approver_.load();
        auto executor = executor_;
        std::thread t([client, this, approver, executor, done]() {
            handle_client(client, approver, executor);
            active_clients_.fetch_sub(1);
            done->store(true, std::memory_order_release);
        });

        reap_finished_threads();
        {
            std::lock_guard<std::mutex> lock(threads_mtx_);
            client_threads_.push_back(ClientThread{std::move(t), done});
        }
    }

    // Graceful shutdown (ARCH-7): stop accepting new connections, then join
    // all in-flight client threads. The approval backend is torn down by
    // stop() (and again idempotently by its destructor), so run() does not
    // re-stop it here.
    closesocket(listen_sock);

    if (approver_retry_thread_.joinable()) approver_retry_thread_.join();

    join_all_threads();

    // Final safety net: ensure no child process outlives the service.
    if (executor_) executor_->terminate_all();

    approver_.store(nullptr);
    executor_.reset();
}

void TcpServer::approver_retry_loop() {
    // Retry create_approver() every 15 s until it succeeds or the server
    // stops.  Sleep in small slices so shutdown stays prompt.
    while (running_) {
        for (int i = 0; i < 30 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!running_ || approver_.load()) return;

        auto cand = create_approver(cfg_);
        if (cand) {
            spdlog::info("approval backend started after retry");
            approver_.store(std::move(cand));
            return;
        }
        spdlog::warn("approval backend retry failed — will try again");
    }
}

void TcpServer::stop() {
    running_ = false;
    // Terminate in-flight child processes and stop the approval backend so any
    // blocked client threads (execution or approval wait) return promptly.
    if (executor_) executor_->terminate_all();
    if (auto approver = approver_.load()) approver->stop();
}

void TcpServer::reap_finished_threads() {
    std::lock_guard<std::mutex> lock(threads_mtx_);
    for (auto it = client_threads_.begin(); it != client_threads_.end();) {
        if (it->done->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = client_threads_.erase(it);
        } else {
            ++it;
        }
    }
}

void TcpServer::join_all_threads() {
    std::lock_guard<std::mutex> lock(threads_mtx_);
    // Bounded wait: a client thread can sit in an approval wait or command
    // execution far longer than the SCM is willing to tolerate during stop.
    // Waiting unconditionally produced processes that hung mid-shutdown while
    // still holding the listening socket (ghost listener, 2026-08-25). After
    // the grace period, detach whatever remains — the process exits right
    // after and the OS reclaims their resources.
    constexpr auto kGrace = std::chrono::seconds(15);
    const auto deadline = std::chrono::steady_clock::now() + kGrace;

    for (auto& ct : client_threads_) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining > std::chrono::milliseconds(0)) {
            // Poll the done flag in small slices until this thread finishes
            // or the global deadline passes.
            while (std::chrono::steady_clock::now() < deadline) {
                if (ct.done->load(std::memory_order_acquire)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!ct.done->load(std::memory_order_acquire)) {
            spdlog::warn("client thread did not finish within shutdown grace — detaching");
            if (ct.thread.joinable()) ct.thread.detach();
        } else if (ct.thread.joinable()) {
            ct.thread.join();
        }
    }
    client_threads_.clear();
}
