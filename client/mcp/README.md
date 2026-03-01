# AdminExecMCP — MCP Client

A Python [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) server that
exposes a single `execute_command` tool to AI assistants such as Claude.  
Commands are forwarded to the **AdminExecMCP Windows service** which executes them
with administrator (SYSTEM) privileges.

## Requirements

- Python 3.12+
- The AdminExecMCP Windows service must be running (see `service/`)

## Installation

```bash
pip install -r requirements.txt
```

Or with conda:

```bash
conda install -c conda-forge mcp
```

## Configuration

| Environment variable  | Default       | Description                         |
|-----------------------|---------------|-------------------------------------|
| `ADMIN_EXEC_HOST`     | `127.0.0.1`   | Service host                        |
| `ADMIN_EXEC_PORT`     | `12380`       | Service TCP port                    |

## MCP host configuration (Claude Desktop)

```json
{
  "mcpServers": {
    "windows-admin-exec": {
      "command": "python",
      "args": ["C:/path/to/client/mcp/main.py"],
      "env": {
        "ADMIN_EXEC_HOST": "127.0.0.1",
        "ADMIN_EXEC_PORT": "12380"
      }
    }
  }
}
```

## Available tools

### `execute_command`

Execute any Windows command with administrator privileges.

| Parameter         | Type    | Default | Description                          |
|-------------------|---------|---------|--------------------------------------|
| `command`         | string  | —       | Command line (`cmd.exe /c …`)        |
| `working_dir`     | string  | `""`    | Working directory (optional)         |
| `timeout_seconds` | integer | `60`    | Execution timeout in seconds         |

**Example prompts:**

- *"List all running Windows services"* → `execute_command("sc query type= all")`
- *"Check disk usage"* → `execute_command("wmic logicaldisk get size,freespace,caption")`
- *"Run a PowerShell script"* → `execute_command("powershell -File C:\\scripts\\setup.ps1")`
