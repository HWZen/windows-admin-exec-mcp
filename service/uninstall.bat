@echo off
:: uninstall.bat — Stop and remove the AdminExecMCP Windows service
:: Must be run as Administrator

set "EXE=%~dp0AdminExecMCP.exe"

if not exist "%EXE%" (
    echo ERROR: AdminExecMCP.exe not found in %~dp0
    exit /b 1
)

"%EXE%" uninstall
if errorlevel 1 (
    echo Uninstallation failed. Run this script as Administrator.
    exit /b 1
)

echo AdminExecMCP service removed.
