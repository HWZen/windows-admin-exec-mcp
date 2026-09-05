"""
AdminExecMCP — MCP server for Windows admin command execution.

This program acts as an MCP server (called by AI/Claude via stdio) and as a
TCP client to the AdminExecMCP Windows service which runs with administrator
privileges.

Usage (in your Claude Desktop / MCP host config):
    {
      "mcpServers": {
        "windows-admin-exec": {
          "command": "python",
          "args": ["/path/to/client/mcp/main.py"],
          "env": {
            "ADMIN_EXEC_HOST": "127.0.0.1",
            "ADMIN_EXEC_PORT": "12380"
          }
        }
      }
    }
"""

import os
from mcp.server.fastmcp import FastMCP
from tcp_client import send_command, ServiceUnavailableError

# ---------------------------------------------------------------------------
# MCP server setup
# ---------------------------------------------------------------------------

mcp = FastMCP(
    "windows-admin-exec",
    instructions=(
        "This MCP server proxies commands to the AdminExecMCP Windows service, "
        "which executes them with full administrator (SYSTEM) privileges. "
        "Use execute_command to run any Windows command or PowerShell script. "
        "Always provide a clear, human-readable 'description' explaining what "
        "the command does and why it is needed — it is shown to the human "
        "approver when approval mode is enabled."
    ),
)

# ---------------------------------------------------------------------------
# Tool definitions
# ---------------------------------------------------------------------------

@mcp.tool()
def execute_command(
    command: str,
    description: str,
    working_dir: str = "",
    timeout_seconds: int = 60,
) -> str:
    """Execute a command on the Windows host with administrator privileges.

    The command is passed to ``cmd.exe /c <command>`` and runs with SYSTEM-level
    (administrator) permissions through the AdminExecMCP Windows service.

    If approval mode is enabled in the service's config.json, the request is
    sent to the configured Telegram/QQ bot and waits for the owner to approve
    it before execution. The ``description`` is displayed to the human approver
    together with the command line, so they can review the request without
    having to decode the raw command themselves.

    Args:
        command: The command line to run, e.g. ``ipconfig /all`` or
                 ``powershell -Command Get-Service``.
        description: Required. One or two plain sentences in the approver's
                     language explaining what this command does and why it is
                     being run, e.g. "查看所有网络适配器的 IP 配置，用于排查
                     网络连通性问题". Do not just repeat the command line.
        working_dir: Working directory for the command (defaults to
                     ``%SystemRoot%\\System32``).
        timeout_seconds: Maximum time to wait for the command to complete
                         (default 60 s; 0 = no timeout).

    Returns:
        Combined stdout / stderr output, or an error description.
    """
    host = os.environ.get("ADMIN_EXEC_HOST", "127.0.0.1")
    port = int(os.environ.get("ADMIN_EXEC_PORT", "12380"))

    try:
        result = send_command(
            host=host,
            port=port,
            command=command,
            description=description,
            working_dir=working_dir,
            timeout_seconds=timeout_seconds,
        )
    except ServiceUnavailableError as exc:
        return f"[SERVICE UNAVAILABLE] {exc}\n\nEnsure the AdminExecMCP Windows service is running."

    if result.get("success"):
        output = result.get("stdout_output", "")
        stderr = result.get("stderr_output", "")
        if stderr:
            output += "\n[STDERR]:\n" + stderr
        return output.strip() or "(Command completed with no output)"

    error = result.get("error_message", "Unknown error")
    exit_code = result.get("exit_code", -1)
    return f"[ERROR] exit_code={exit_code}\n{error}"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run()
