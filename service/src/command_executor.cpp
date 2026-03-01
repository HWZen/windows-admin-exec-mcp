#include "command_executor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <array>
#include <chrono>

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
// Uses MultiByteToWideChar with MB_ERR_INVALID_CHARS: any invalid byte
// causes the call to fail and return 0, so `r > 0` means "all bytes are
// valid UTF-8".  Empty strings are considered valid.
bool is_valid_utf8(const std::string& s) {
    if (s.empty()) return true;
    int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    return r > 0;
}

// Convert a GBK (codepage 936) encoded string to UTF-8.
// If the bytes cannot be decoded as GBK (which should be rare), non-ASCII
// bytes are replaced with '?' to guarantee the returned string is always
// valid UTF-8.
std::string gbk_to_utf8(const std::string& s) {
    if (s.empty()) return {};
    // GBK → UTF-16
    int wlen = MultiByteToWideChar(936, 0,
                                   s.data(), static_cast<int>(s.size()),
                                   nullptr, 0);
    if (wlen > 0) {
        std::wstring ws(wlen, L'\0');
        MultiByteToWideChar(936, 0, s.data(), static_cast<int>(s.size()), ws.data(), wlen);
        // UTF-16 → UTF-8
        return to_utf8(ws);
    }
    // Fallback: bytes are neither valid UTF-8 nor valid GBK.
    // Replace every non-ASCII byte with '?' so the result is valid UTF-8.
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s) {
        result += (c < 0x80) ? static_cast<char>(c) : '?';
    }
    return result;
}

// Guarantee the returned string is valid UTF-8.
// Only UTF-8 and GBK (codepage 936) are considered as input encodings.
// If the input is not valid UTF-8 it is assumed to be GBK and converted
// accordingly.  This matches the requirement that only these two encodings
// need to be supported.
std::string ensure_utf8(const std::string& s) {
    if (is_valid_utf8(s)) return s;
    return gbk_to_utf8(s);
}

} // anonymous namespace

void execute_command(const CommandRequest& req, CommandResponse& resp) {
    resp.id = req.id;

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
        return;
    }

    // Ensure the read ends are not inherited by the child
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Open the NUL device as an inheritable, read-only handle for the child's
    // stdin.  Using NUL ensures the child has a valid stdin fd but receives
    // immediate EOF on any read attempt — no input is forwarded from the
    // service process.
    HANDLE nul_handle = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,           // bInheritHandle = TRUE
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (nul_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        resp.error_message = "Failed to open NUL device for child stdin";
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
    // Also close the NUL handle — the child already inherited its own copy.
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (nul_handle != INVALID_HANDLE_VALUE) CloseHandle(nul_handle);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        DWORD err = GetLastError();
        resp.error_message = "CreateProcess failed, error code: " + std::to_string(err);
        return;
    }

    // Wait for the process to finish, draining stdout/stderr in 1-second
    // slices to avoid pipe buffer deadlocks.  The child may block on a full
    // pipe write if the parent doesn't read; polling with PeekNamedPipe
    // prevents that while still respecting the overall timeout.
    const bool infinite_wait = (req.timeout_seconds == 0);
    const DWORD timeout_ms   = infinite_wait
        ? 0u
        : static_cast<DWORD>(req.timeout_seconds) * 1000u;

    auto start_time = std::chrono::steady_clock::now();
    bool timed_out  = false;

    while (true) {
        DWORD slice = WaitForSingleObject(pi.hProcess, 1000); // 1-second poll slice
        // Drain whatever output became available during this slice.
        peek_and_read(stdout_read, resp.stdout_output);
        peek_and_read(stderr_read, resp.stderr_output);

        if (slice == WAIT_OBJECT_0) {
            break; // Process finished
        }

        // WAIT_TIMEOUT: check against the total configured timeout.
        if (!infinite_wait) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (static_cast<DWORD>(elapsed_ms) >= timeout_ms) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                // Final drain after forced termination.
                peek_and_read(stdout_read, resp.stdout_output);
                peek_and_read(stderr_read, resp.stderr_output);
                timed_out = true;
                break;
            }
        }
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    resp.exit_code = static_cast<int>(exit_code);

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
}
