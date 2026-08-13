#include "service.h"
#include "tcp_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <memory>

// ---------------------------------------------------------------------------
// Global state needed by the SCM callbacks
// ---------------------------------------------------------------------------

static constexpr wchar_t kServiceName[] = L"AdminExecMCP";
static constexpr wchar_t kServiceDisplayName[] = L"Admin Exec MCP Service";
static constexpr wchar_t kServiceDescription[] =
    L"Executes commands with administrator privileges on behalf of the AdminExecMCP clients.";

static SERVICE_STATUS          g_status{};
static SERVICE_STATUS_HANDLE   g_status_handle{};
static TcpServer*              g_server_ptr = nullptr;

// ---------------------------------------------------------------------------
// Helper: report current service status to SCM
// ---------------------------------------------------------------------------

static void report_status(DWORD state,
                           DWORD exit_code = NO_ERROR,
                           DWORD wait_hint = 0) {
    static DWORD checkpoint = 1;
    g_status.dwCurrentState  = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint      = wait_hint;
    g_status.dwCheckPoint    = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_status_handle, &g_status);
}

// ---------------------------------------------------------------------------
// SCM control handler
// ---------------------------------------------------------------------------

static VOID WINAPI service_ctrl_handler(DWORD ctrl) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report_status(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        if (g_server_ptr) g_server_ptr->stop();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Windows service entry point (called by SCM)
// ---------------------------------------------------------------------------

// We use a global ServiceConfig pointer so the WINAPI callback can reach it.
static const ServiceConfig* g_cfg_ptr = nullptr;

static VOID WINAPI service_entry(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted        = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    g_status_handle = RegisterServiceCtrlHandlerW(kServiceName, service_ctrl_handler);
    if (!g_status_handle) return;

    report_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    TcpServer server(*g_cfg_ptr);
    g_server_ptr = &server;

    report_status(SERVICE_RUNNING);

    // run() blocks until stop() is called from service_ctrl_handler
    server.run();

    report_status(SERVICE_STOPPED);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void service_main(const ServiceConfig& cfg) {
    g_cfg_ptr = &cfg;
    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        { const_cast<LPWSTR>(kServiceName), service_entry },
        { nullptr, nullptr }
    };
    StartServiceCtrlDispatcherW(dispatch_table);
}

bool install_service(const std::wstring& exe_path) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;

    // Build command line with "run" flag so the binary knows it's the service
    std::wstring cmd = L"\"" + exe_path + L"\" run";

    SC_HANDLE svc = CreateServiceW(
        scm,
        kServiceName,
        kServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmd.c_str(),
        nullptr,  // no load ordering group
        nullptr,  // no tag
        nullptr,  // no dependencies
        nullptr,  // LocalSystem account
        nullptr   // no password
    );

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) return true; // Already installed
        return false;
    }

    // Set description
    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kServiceDescription)};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Start the service immediately after installation
    StartServiceW(svc, 0, nullptr);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool uninstall_service() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        CloseServiceHandle(scm);
        return (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST);
    }

    // Stop the service if running
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    // Wait up to 10 s for it to stop
    for (int i = 0; i < 20; ++i) {
        QueryServiceStatus(svc, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(500);
    }

    BOOL deleted = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return deleted != FALSE;
}
