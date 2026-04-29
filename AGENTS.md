# AGENTS.md

This repository has two main implementation surfaces and one test surface:

- `service/`: C++20 Windows service (`AdminExecMCP`) built with CMake and vcpkg.
- `client/mcp/`: Python 3.12 MCP stdio server that forwards requests to the Windows service over TCP.
- `client/openclaw/`: Python integration that reuses the same TCP protocol.
- `tests/`: Python tests for the TCP client protocol behavior.

## Start Here

- Read [README.md](README.md) for architecture, install flow, and protocol overview.
- Read [service/CMakeLists.txt](service/CMakeLists.txt) before changing C++ build settings or dependencies.
- Read [service/config.example.json](service/config.example.json) before changing runtime configuration behavior.
- Read [service/src/protocol.h](service/src/protocol.h) before changing request or response payloads.
- Read [client/mcp/main.py](client/mcp/main.py) and [client/mcp/tcp_client.py](client/mcp/tcp_client.py) before changing Python client behavior.
- Read [tests/test_tcp_client.py](tests/test_tcp_client.py) before editing TCP framing or client error handling.

## Working Rules

- Prefer changes in source files under `service/src/`, `client/mcp/`, `client/openclaw/`, and `tests/`.
- Do not edit generated or local-build directories unless the user explicitly asks: `service/build/`, `service/cmake-build-debug/`, `.cache/`.
- Keep the wire protocol aligned across C++ and Python. Any message shape or framing change requires checking both implementations and tests.
- Keep Windows-specific behavior explicit. This project intentionally uses the Windows Service Control Manager, `cmd.exe /c`, Winsock, and administrator/SYSTEM execution.
- Preserve the default security posture unless asked otherwise: loopback bind (`127.0.0.1`) and optional approval flow.

## Build And Test

- C++ service configure/build:

```bat
cd service
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

- Python test path:

```bat
pytest tests/
```

- Python client setup when needed:

```bat
pip install -r client/mcp/requirements.txt
```

## Validation Expectations

- For changes under `tests/` or `client/mcp/`, run `pytest tests/` when possible.
- For changes under `service/src/`, prefer a narrow CMake build of the service over broad repository checks.
- If you change protocol fields, validate both the Python tests and the corresponding C++/Python serialization sites.
- If you change service install or runtime behavior, note whether validation was limited because service install/uninstall and `console` mode require Administrator privileges on Windows.

## Environment Pitfalls

- C++ builds assume Windows plus vcpkg. `VCPKG_ROOT` must be set or `-DCMAKE_TOOLCHAIN_FILE=...` must be passed.
- Service install and uninstall require an elevated shell.
- The service expects `config.json` next to `AdminExecMCP.exe`; `service/config.example.json` is the source template.
- The default TCP endpoint is `127.0.0.1:12380`. Client-side overrides come from `ADMIN_EXEC_HOST` and `ADMIN_EXEC_PORT`.
- Tests are designed to run without the Windows service by mocking the TCP layer. Prefer those tests for fast validation.

## Change Patterns

- Protocol or transport work usually touches `service/src/protocol.h`, `service/src/tcp_server.cpp`, `client/mcp/tcp_client.py`, and [tests/test_tcp_client.py](tests/test_tcp_client.py).
- Command execution behavior usually lives in `service/src/command_executor.cpp` and may surface through the Python MCP tool in [client/mcp/main.py](client/mcp/main.py).
- Approval-flow changes usually involve `service/src/telegram_approver.cpp`, configuration parsing, and the config example file.

## Documentation Strategy

- Link to existing project docs instead of copying them into new agent instructions.
- If a task adds a new developer workflow or validation command, update [README.md](README.md) when that information is user-facing rather than expanding this file with long prose.