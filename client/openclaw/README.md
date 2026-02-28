# AdminExecMCP — OpenClaw Skill

An [OpenClaw](https://github.com/search?q=openClaw&type=repositories) skill that
exposes Windows administrator command execution.  
Commands are forwarded to the **AdminExecMCP Windows service** and executed with
administrator (SYSTEM) privileges.

## Requirements

- Python 3.12+
- The AdminExecMCP Windows service must be running (see `service/`)
- `client/mcp/tcp_client.py` must be importable (shared with the MCP client)

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

## Loading the skill in OpenClaw

```python
import importlib.util, sys, os

spec = importlib.util.spec_from_file_location(
    "admin_exec_skill",
    r"C:\path\to\client\openclaw\skill.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

# Register skills with your OpenClaw agent
for skill in module.SKILLS:
    agent.register_skill(skill)
```

## Skill: `execute_command`

| Parameter         | Type    | Default | Description                          |
|-------------------|---------|---------|--------------------------------------|
| `command`         | string  | —       | Command line (`cmd.exe /c …`)        |
| `working_dir`     | string  | `""`    | Working directory (optional)         |
| `timeout_seconds` | integer | `60`    | Execution timeout in seconds         |
