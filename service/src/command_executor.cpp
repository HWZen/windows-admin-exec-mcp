#include "command_executor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace {

// Drain all bytes currently available in a named pipe, appending to `out`.
// Uses PeekNamedPipe so it never blocks when the pipe is empty.
void peek_and_read(HANDLE h, std::string& out) {
    DWORD avail = 0;
    while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        std::array<char, 4096> buf{};
        DWORD to_read = avail < static_cast<DWORD>(buf.size())
                        ? avail : static_cast<DWORD>(buf.size());
        DWORD read = 0;
        if (!ReadFile(h, buf.data(), to_read, &read, nullptr) || read == 0) break;
        out.append(buf.data(), read);
    }
}

// Convert a narrow string to a wide string (UTF-8 source).
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

// Convert a wide string to a narrow UTF-8 string.
std::string to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

// Return true when every byte in `s` forms a valid UTF-8 sequence.
bool is_valid_utf8(const std::string& s) {
    if (s.empty()) return true;
    int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    return r > 0;
}

// Convert a GBK (codepage 936) encoded string to UTF-8.
std::string gbk_to_utf8(const std::string& s) {
    if (s.empty()) return {};
    int wlen = MultiByteToWideChar(936, 0,
                                   s.data(), static_cast<int>(s.size()),
                                   nullptr, 0);
    if (wlen > 0) {
        std::wstring ws(wlen, L'\0');
        MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), ws.data(), wlen);
        return to_utf8(ws);
    }
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s) {
        result += (c < 0x80) ? static_cast<char>(c) : '?';
    }
    return result;
}

// Guarantee the returned string is valid UTF-8 (UTF-8 or GBK input).
std::string ensure_utf8(const std::string& s) {
    if (is_valid_utf8(s)) return s;
    return gbk_to_utf8(s);
}

} // anonymous namespace

void CommandExecutor::execute(const CommandRequest& req, CommandResponse& resp) {
    resp.id = req.id;

    // Audit log (SEC-L3): record what is about to be executed.
    spdlog::info("AUDIT: execute id={} command=\"{}\" working_dir=\"{}\" timeout={}s",
                 req.id, req.command, req.working_dir, req.timeout_seconds);

    auto start_time = std::chrono::steady_clock::now();

    // Build command line: cmd.exe /c <user_command>
    std::wstring cmdline = L"cmd.exe /c " + to_wide(req.command);

    // Determine working directory (NULL == inherit service working dir)
    std::wstring wdir_str;
    const wchar_t* wdir_ptr = nullptr;
    if (!req.working_dir.empty()) {
        wdir_str = to_wide(req.working_dir);
        wdir_ptr = wdir_str.c_str();
    }

    // Create anonymous pipes for stdout and stderr redirection
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        resp.error_message = "Failed to create pipes";
        spdlog::error("AUDIT: id={} failed: {}", req.id, resp.error_message);
        return;
    }

    // Ensure the read ends are not inherited by the child
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Open the NUL device as an inheritable, read-only handle for the child's stdin.
    HANDLE nul_handle = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (nul_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        resp.error_message = "Failed to open NUL device for child stdin";
        spdlog::error("AUDIT: id={} failed: {}", req.id, resp.error_message);
        return;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = stdout_write;
    si.hStdError  = stderr_write;
    si.hStdInput  = nul_handle;
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    BOOL created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,   // inherit handles
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        wdir_ptr,
        &si,
        &pi
    );

    // Close the write ends in the parent immediately after spawning.
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (nul_handle != INVALID_HANDLE_VALUE) CloseHandle(nul_handle);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        DWORD err = GetLastError();
        resp.error_message = "CreateProcess failed, error code: " + std::to_string(err);
        spdlog::error("AUDIT: id={} failed: {}", req.id, resp.error_message);
        return;
    }

    // Register the child process handle so shutdown can terminate it (ARCH-7).
    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_processes_.push_back(pi.hProcess);
    }

    const bool infinite_wait = (req.timeout_seconds == 0);
    const DWORD timeout_ms   = infinite_wait
        ? 0u
        : static_cast<DWORD>(req.timeout_seconds) * 1000u;

    bool timed_out  = false;

    while (true) {
        DWORD slice = WaitForSingleObject(pi.hProcess, 1000); // 1-second poll slice
        peek_and_read(stdout_read, resp.stdout_output);
        peek_and_read(stderr_read, resp.stderr_output);

        if (slice == WAIT_OBJECT_0) {
            break; // Process finished
        }

        if (!infinite_wait) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (static_cast<DWORD>(elapsed_ms) >= timeout_ms) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                peek_and_read(stdout_read, resp.stdout_output);
                peek_and_read(stderr_read, resp.stderr_output);
                timed_out = true;
                break;
            }
        }
    }

    // Unregister the child process handle now that it has finished.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = std::find(active_processes_.begin(), active_processes_.end(), pi.hProcess);
        if (it != active_processes_.end()) active_processes_.erase(it);
    }

    // Check the return value of GetExitCodeProcess; a failure here must not be
    // misreported as a successful exit code of 0 (BUG-L2).
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        spdlog::error("AUDIT: id={} GetExitCodeProcess failed (error {})", req.id, GetLastError());
        resp.exit_code = -1;
    } else {
        resp.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Convert accumulated pipe output to UTF-8 (GBK if not already valid UTF-8).
    resp.stdout_output = ensure_utf8(resp.stdout_output);
    resp.stderr_output = ensure_utf8(resp.stderr_output);

    if (timed_out) {
        resp.error_message = "Command timed out after " + std::to_string(req.timeout_seconds) + " seconds";
        resp.success = false;
    } else {
        resp.success = true;
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    spdlog::info("AUDIT: complete id={} success={} exit_code={} stdout_bytes={} stderr_bytes={} duration_ms={}",
                 req.id, resp.success, resp.exit_code,
                 resp.stdout_output.size(), resp.stderr_output.size(), elapsed_ms);
}

void CommandExecutor::terminate_all() {
    // Hold the lock while terminating so a concurrently-finishing execute()
    // cannot close a handle between our read and TerminateProcess.
    std::lock_guard<std::mutex> lock(mtx_);
    for (void* h : active_processes_) {
        if (h) {
            TerminateProcess(static_cast<HANDLE>(h), 1);
        }
    }
}
