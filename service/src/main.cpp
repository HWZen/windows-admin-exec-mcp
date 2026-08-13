#include "config.h"
#include "service.h"
#include "tcp_server.h"
#include "logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace {

// Locate a file path next to the executable (UTF-8).
std::string path_next_to_exe(const std::wstring& filename) {
    wchar_t exe_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::path p = fs::path(exe_buf).parent_path() / filename;
    int len = WideCharToMultiByte(CP_UTF8, 0,
        p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        p.c_str(), -1, result.data(), len, nullptr, nullptr);
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

    try {
        std::wcout.imbue(std::locale("chs.UTF-8"));
    } catch (...) {
    }

    // Initialise the logger first (default level) so config-parse warnings
    // from load_config() are captured in the log file.
    std::string log_path = path_next_to_exe(L"AdminExecMCP.log");
    init_logger(log_path, "info");

    // Load config, then apply the configured log level on top of the default.
    std::string cfg_path = path_next_to_exe(L"config.json");
    ServiceConfig cfg = load_config(cfg_path);
    cfg.config_path = cfg_path;

    auto lvl = spdlog::level::from_str(cfg.log_level);
    if (lvl == spdlog::level::off && cfg.log_level != "off") {
        lvl = spdlog::level::info;
    }
    spdlog::set_level(lvl);

    spdlog::info("AdminExecMCP starting (log: {}, level: {})", log_path, cfg.log_level);

    if (argc < 2) {
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
            spdlog::info("Service installed and started");
            return 0;
        } else {
            std::wcerr << L"Failed to install service (error " << GetLastError() << L").\n"
                       << L"Run as Administrator.\n";
            spdlog::error("Failed to install service (error {})", GetLastError());
            return 1;
        }
    }

    if (cmd == L"uninstall") {
        if (uninstall_service()) {
            std::wcout << L"Service uninstalled successfully.\n";
            spdlog::info("Service uninstalled");
            return 0;
        } else {
            std::wcerr << L"Failed to uninstall service (error " << GetLastError() << L").\n";
            spdlog::error("Failed to uninstall service (error {})", GetLastError());
            return 1;
        }
    }

    if (cmd == L"run") {
        spdlog::info("Starting as Windows service (SCM mode)");
        service_main(cfg);
        return 0;
    }

    if (cmd == L"console") {
        std::wcout << L"Running in console mode on "
                   << std::wstring(cfg.bind_address.begin(), cfg.bind_address.end())
                   << L":" << cfg.port << L"\n"
                   << L"Press Ctrl+C to stop.\n";
        std::wcout.flush();
        spdlog::info("Console mode on {}:{}", cfg.bind_address, cfg.port);
        TcpServer server(cfg);
        server.run();
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
