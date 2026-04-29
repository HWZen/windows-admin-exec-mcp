import json
import os
import socket
import sys
import uuid


REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
TCP_CLIENT_DIR = os.path.join(REPO_ROOT, "client", "mcp")

if TCP_CLIENT_DIR not in sys.path:
    sys.path.insert(0, TCP_CLIENT_DIR)

from tcp_client import _recv_message, _send_message, ServiceUnavailableError


def send_raw_command(
    host: str,
    port: int,
    command: str,
    working_dir: str = "",
    timeout_seconds: int = 60,
    connect_timeout: float = 5.0,
) -> str:
    request = {
        "id": str(uuid.uuid4()),
        "command": command,
        "working_dir": working_dir,
        "timeout_seconds": timeout_seconds,
    }
    payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
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

    return response_bytes.decode("utf-8")


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: python admin-test.py <command>", file=sys.stderr)
        return 2

    host = os.environ.get("ADMIN_EXEC_HOST", "127.0.0.1")
    port = int(os.environ.get("ADMIN_EXEC_PORT", "12380"))
    command = " ".join(sys.argv[1:])

    try:
        raw_response = send_raw_command(host=host, port=port, command=command)
    except ServiceUnavailableError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(raw_response)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())