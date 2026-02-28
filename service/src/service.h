#pragma once

#include "config.h"

// Set up and register the Windows service control handler.
// Called once from main() when running as a Windows service.
void service_main(const ServiceConfig& cfg);

// Install this executable as an auto-start Windows service named "AdminExecMCP".
// Must be run with administrative privileges.
bool install_service(const std::wstring& exe_path);

// Remove the "AdminExecMCP" service from the Service Control Manager.
// Must be run with administrative privileges.
bool uninstall_service();
