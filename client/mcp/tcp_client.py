"""
TCP client helper for the AdminExecMCP Windows service.

Protocol: each message is framed as [4-byte big-endian length][JSON body].
"""

import json
import socket
import struct
import uuid
from typing import Any


class ServiceUnavailableError(OSError):
    """Raised when the AdminExecMCP service cannot be reached."""


def _send_message(sock: socket.socket, payload: bytes) -> None:
    """Send a length-prefixed message."""
    header = struct.pack(">I", len(payload))
    sock.sendall(header + payload)


def _recv_message(sock: socket.socket) -> bytes:
    """Receive a length-prefixed message."""
    header = _recv_exactly(sock, 4)
    (length,) = struct.unpack(">I", header)
    if length == 0 or length > 16 * 1024 * 1024:
        raise ValueError(f"Invalid message length: {length}")
    return _recv_exactly(sock, length)


def _recv_exactly(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes from the socket."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("Connection closed by service")
        buf.extend(chunk)
    return bytes(buf)


def send_command(
    host: str,
    port: int,
    command: str,
    working_dir: str = "",
    timeout_seconds: int = 60,
    connect_timeout: float = 5.0,
) -> dict[str, Any]:
    """Send an execution request to the AdminExecMCP service and return the response dict.

    Args:
        host: Service hostname (default ``127.0.0.1``).
        port: Service TCP port (default ``12380``).
        command: Command line to execute.
        working_dir: Optional working directory.
        timeout_seconds: Command execution timeout in seconds.
        connect_timeout: Seconds to wait for the TCP connection.

    Returns:
        Parsed JSON response dict with keys:
        ``id``, ``success``, ``stdout_output``, ``stderr_output``,
        ``exit_code``, ``error_message``.

    Raises:
        ServiceUnavailableError: If the service cannot be connected to.
        ValueError: On protocol framing or JSON errors.
    """
    request = {
        "id": str(uuid.uuid4()),
        "command": command,
        "working_dir": working_dir,
        "timeout_seconds": timeout_seconds,
    }
    payload = json.dumps(request, ensure_ascii=False).encode("utf-8")

    # The socket read timeout must be generous enough to cover the command
    # execution timeout plus some overhead.
    socket_timeout = float(timeout_seconds) + 30.0

    try:
        with socket.create_connection((host, port), timeout=connect_timeout) as sock:
            sock.settimeout(socket_timeout)
            _send_message(sock, payload)
            response_bytes = _recv_message(sock)
    except (ConnectionRefusedError, OSError) as exc:
        raise ServiceUnavailableError(
            f"Cannot connect to AdminExecMCP service at {host}:{port}: {exc}"
        ) from exc

    return json.loads(response_bytes.decode("utf-8"))
