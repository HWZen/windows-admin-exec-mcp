#include "command_executor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <array>

namespace {

// Read all data available from a non-blocking anonymous pipe handle.
std::string drain_pipe(HANDLE h) {
    std::string result;
    std::array<char, 4096> buf{};
    DWORD read = 0;
    while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read > 0) {
        result.append(buf.data(), read);
    }
    return result;
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

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = stdout_write;
    si.hStdError  = stderr_write;
    si.hStdInput  = INVALID_HANDLE_VALUE;
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

    // Close the write ends in the parent immediately after spawning
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        DWORD err = GetLastError();
        resp.error_message = "CreateProcess failed, error code: " + std::to_string(err);
        return;
    }

    // Wait for the process to finish (or timeout)
    DWORD timeout_ms = (req.timeout_seconds == 0)
        ? INFINITE
        : static_cast<DWORD>(req.timeout_seconds) * 1000u;

    DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);

    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    resp.stdout_output = ensure_utf8(drain_pipe(stdout_read));
    resp.stderr_output = ensure_utf8(drain_pipe(stderr_read));

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    resp.exit_code = static_cast<int>(exit_code);

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (wait_result == WAIT_TIMEOUT) {
        resp.error_message = "Command timed out after " + std::to_string(req.timeout_seconds) + " seconds";
        resp.success = false;
    } else {
        resp.success = true;
    }
}
