@echo off
:: install.bat — Install and start the AdminExecMCP Windows service
:: Must be run as Administrator

set "EXE=%~dp0AdminExecMCP.exe"

if not exist "%EXE%" (
    echo ERROR: AdminExecMCP.exe not found in %~dp0
    echo Please build the project first.
    exit /b 1
)

:: Copy example config if no config.json exists
if not exist "%~dp0config.json" (
    if exist "%~dp0config.example.json" (
        copy "%~dp0config.example.json" "%~dp0config.json"
        echo Copied config.example.json to config.json
        echo Please review and update config.json before the service runs commands.
    )
)

"%EXE%" install
if errorlevel 1 (
    echo Installation failed. Run this script as Administrator.
    exit /b 1
)

echo.
echo AdminExecMCP service installed and started.
echo Configure approval settings in: %~dp0config.json
