# windows-admin-exec-mcp

MCP for Windows systems to execute instructions with administrator privileges.  
适用于Windows系统的以管理员权限执行指令的 MCP。

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  AI Assistant (Claude / any MCP host)                   │
│     ↓ MCP stdio                                         │
│  Python MCP Server  (client/mcp/main.py)                │
│     ↓ TCP JSON (localhost:12380)                        │
│  C++ Windows Service  (service/)                        │
│     • Runs as LocalSystem (administrator)               │
│     • Optional Telegram / QQ bot approval gate          │
│     • Executes cmd.exe /c <command>                     │
│     • Returns stdout / stderr / exit code               │
└─────────────────────────────────────────────────────────┘

Optional alternative client:
  Python OpenClaw Skill  (client/openclaw/skill.py)
     ↓ same TCP JSON protocol
  C++ Windows Service
```

### Components

| Component | Language / Tech | Location |
|-----------|----------------|----------|
| Windows service | C++20, CMake, vcpkg | `service/` |
| MCP server (AI client) | Python 3.12, `mcp` | `client/mcp/` |
| OpenClaw skill | Python 3.12 | `client/openclaw/` |

---

## IPC Protocol

All messages between the Python clients and the C++ service are framed as:

```
[4-byte big-endian length][UTF-8 JSON body]
```

**Request** (client → service):
```json
{
  "id": "uuid4",
  "command": "ipconfig /all",
  "working_dir": "",
  "timeout_seconds": 60
}
```

**Response** (service → client):
```json
{
  "id": "uuid4",
  "success": true,
  "stdout_output": "...",
  "stderr_output": "",
  "exit_code": 0,
  "error_message": ""
}
```

---

## Quick Start

### 1. Build the Windows Service

Requirements: Visual Studio 2022, CMake ≥ 3.25, vcpkg.

```bat
cd service
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

### 2. Install the Service

```bat
cd service
copy build\Release\AdminExecMCP.exe .
copy config.example.json config.json
REM Edit config.json if you want Telegram approval
install.bat
```

> **Note**: `install.bat` must be run as Administrator.  
> The service registers itself as `AdminExecMCP` and starts automatically on boot.

### 3. Configure the Python MCP client

```bash
pip install -r client/mcp/requirements.txt
```

Add to your Claude Desktop / MCP host config (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "windows-admin-exec": {
      "command": "python",
      "args": ["C:/path/to/client/mcp/main.py"]
    }
  }
}
```

### 4. (Optional) Enable Telegram Approval

Edit `config.json` next to `AdminExecMCP.exe`:

```json
{
  "approval": {
    "enabled": true,
    "type": "telegram",
    "telegram": {
      "bot_token": "123456:ABC-DEF...",
      "chat_id": "987654321",
      "timeout_seconds": 300
    }
  }
}
```

Restart the service after editing:

```bat
net stop AdminExecMCP && net start AdminExecMCP
```

When the AI calls `execute_command`, the service will send a Telegram message like:

```
🔐 AdminExecMCP — Approval Required

Command:
  ipconfig /all

Reply:
  /approve_<uuid>  — to approve
  /deny_<uuid>     — to deny
```

### 5. (Optional) Enable QQ Bot Approval

Edit `config.json` next to `AdminExecMCP.exe`:

```json
{
  "approval": {
    "enabled": true,
    "type": "qq",
    "qq": {
      "app_id": "YOUR_QQ_BOT_APP_ID",
      "app_secret": "YOUR_QQ_BOT_APP_SECRET",
      "user_openid": "",
      "timeout_seconds": 300
    }
  }
}
```

Restart the service after editing:

```bat
net stop AdminExecMCP && net start AdminExecMCP
```

When the AI calls `execute_command`, the service will send a QQ **single-chat
(C2C)** message with inline-keyboard Approve / Deny buttons.  The service
maintains a persistent WebSocket connection to the QQ bot gateway to receive
button-click callbacks in real time.

> **Prerequisites**:
> - Register a bot on the [QQ Open Platform](https://q.qq.com/) and obtain the
>   `AppID` and `AppSecret`.
> - Add the bot as a QQ friend.
> - **Auto-capture**: if `user_openid` is left empty, the service will
>   automatically capture it from the first message you send to the bot.
>   Simply send any message (e.g. "hello") to the bot after starting the
>   service.  The captured `user_openid` will be used for all subsequent
>   approval notifications.
> - To find your `user_openid` manually, check the service log output after
>   sending a message to the bot.

### 6. (Optional) OpenClaw Skill

```python
import importlib.util
spec = importlib.util.spec_from_file_location("skill", r"C:\path\to\client\openclaw\skill.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
for skill in module.SKILLS:
    agent.register_skill(skill)
```

---

## Service Management

| Action | Command (run as Admin) |
|--------|----------------------|
| Install & start | `AdminExecMCP.exe install` |
| Remove | `AdminExecMCP.exe uninstall` |
| Debug (console) | `AdminExecMCP.exe console` |

---

## Security Considerations

- The service binds **only to `127.0.0.1`** (localhost) by default; it is not
  accessible from the network.
- Enable **Telegram or QQ bot approval** (`approval.enabled = true`) to require explicit
  human sign-off for every command the AI attempts to run.
- The service runs as `LocalSystem`. Commands it executes inherit this token and
  have full administrator access to the local machine.
- Restrict who can connect to the MCP server in your AI host configuration.

---

## Running the Tests

```bash
pip install pytest mcp
pytest tests/
```

---

## Tech Stack

| Layer | Technology |
|-------|-----------|
| C++ service | C++20, CMake 3.25+, VS 2022, [vcpkg](https://vcpkg.io) |
| C++ dependencies | [nlohmann/json](https://github.com/nlohmann/json), [libcurl](https://curl.se/libcurl/), WinHTTP (WebSocket) |
| Python clients | Python 3.12, [mcp](https://pypi.org/project/mcp/) |
| Telegram integration | Telegram Bot API via libcurl (C++) |
| QQ bot integration | QQ Bot API v2 via libcurl (HTTP) + WinHTTP (WebSocket gateway) |
