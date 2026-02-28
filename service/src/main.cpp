#include "config.h"
#include "service.h"
#include "tcp_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Locate the config file next to the executable.
std::string find_config_path() {
    wchar_t exe_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::path cfg = fs::path(exe_buf).parent_path() / L"config.json";
    // Convert to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0,
        cfg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        cfg.c_str(), -1, result.data(), len, nullptr, nullptr);
    return result;
}

void print_usage(const wchar_t* prog) {
    std::wcout << L"Usage: " << prog << L" [install|uninstall|run|console]\n"
               << L"  install    Install and start the Windows service (requires admin)\n"
               << L"  uninstall  Stop and remove the Windows service (requires admin)\n"
               << L"  run        Start as a Windows service (called by SCM)\n"
               << L"  console    Run interactively in the current console (for testing)\n";
}

} // anonymous namespace

int wmain(int argc, wchar_t* argv[]) {
    ServiceConfig cfg = load_config(find_config_path());

    if (argc < 2) {
        // If launched without arguments it may be an SCM start —
        // try service mode first; fall through to usage on failure.
        service_main(cfg);
        print_usage(argv[0]);
        return 0;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"install") {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (install_service(exe_path)) {
            std::wcout << L"Service installed and started successfully.\n";
            return 0;
        } else {
            std::wcerr << L"Failed to install service (error " << GetLastError() << L").\n"
                       << L"Run as Administrator.\n";
            return 1;
        }
    }

    if (cmd == L"uninstall") {
        if (uninstall_service()) {
            std::wcout << L"Service uninstalled successfully.\n";
            return 0;
        } else {
            std::wcerr << L"Failed to uninstall service (error " << GetLastError() << L").\n";
            return 1;
        }
    }

    if (cmd == L"run") {
        // Started by SCM
        service_main(cfg);
        return 0;
    }

    if (cmd == L"console") {
        // Interactive / debug mode
        std::wcout << L"Running in console mode on "
                   << std::wstring(cfg.bind_address.begin(), cfg.bind_address.end())
                   << L":" << cfg.port << L"\n"
                   << L"Press Ctrl+C to stop.\n";
        TcpServer server(cfg);
        server.run();
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
