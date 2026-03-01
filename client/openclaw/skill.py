"""
AdminExecMCP — OpenClaw Skill

Exposes Windows administrator command execution as an OpenClaw skill.
The skill connects to the AdminExecMCP Windows service via TCP and returns
the command output.

OpenClaw skill interface follows the convention used by the openClaw agent
framework: a module-level ``SKILLS`` list of callable objects, each with
``name``, ``description``, ``parameters`` metadata and a ``__call__`` method.
"""

import os
import sys
from typing import Any

# Allow importing tcp_client from the sibling mcp package when both clients
# share the same installation layout.
_here = os.path.dirname(os.path.abspath(__file__))
_mcp_dir = os.path.join(_here, "..", "mcp")
if _mcp_dir not in sys.path:
    sys.path.insert(0, _mcp_dir)

from tcp_client import send_command, ServiceUnavailableError  # noqa: E402


# ---------------------------------------------------------------------------
# Skill implementation
# ---------------------------------------------------------------------------

class ExecuteCommandSkill:
    """OpenClaw skill: execute a Windows command with administrator privileges."""

    name = "execute_command"
    description = (
        "Execute a command on the Windows host with full administrator (SYSTEM) "
        "privileges via the AdminExecMCP service. "
        "If Telegram approval is configured, the owner must approve the request "
        "before the command runs."
    )
    parameters = {
        "type": "object",
        "properties": {
            "command": {
                "type": "string",
                "description": "Command line to execute (passed to cmd.exe /c).",
            },
            "working_dir": {
                "type": "string",
                "description": "Working directory (optional).",
                "default": "",
            },
            "timeout_seconds": {
                "type": "integer",
                "description": "Execution timeout in seconds (default 60).",
                "default": 60,
            },
        },
        "required": ["command"],
    }

    def __call__(
        self,
        command: str,
        working_dir: str = "",
        timeout_seconds: int = 60,
        **_kwargs: Any,
    ) -> str:
        """Invoke the skill and return the command output as a string."""
        host = os.environ.get("ADMIN_EXEC_HOST", "127.0.0.1")
        port = int(os.environ.get("ADMIN_EXEC_PORT", "12380"))

        try:
            result = send_command(
                host=host,
                port=port,
                command=command,
                working_dir=working_dir,
                timeout_seconds=timeout_seconds,
            )
        except ServiceUnavailableError as exc:
            return (
                f"[SERVICE UNAVAILABLE] {exc}\n"
                "Ensure the AdminExecMCP Windows service is running."
            )

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
# OpenClaw skill registry
# ---------------------------------------------------------------------------

SKILLS: list[Any] = [ExecuteCommandSkill()]
