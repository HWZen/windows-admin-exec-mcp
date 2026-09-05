"""
Unit tests for the AdminExecMCP TCP client (tcp_client.py).

These tests mock the network layer so they run without the Windows service.
Run with: pytest tests/
"""

import json
import struct
import threading
import socket
import sys
import os

import pytest

# Add client/mcp to path so tcp_client can be imported directly
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client", "mcp"))

from tcp_client import send_command, ServiceUnavailableError


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _frame(data: dict) -> bytes:
    """Encode a dict as a length-prefixed JSON message (server→client direction)."""
    payload = json.dumps(data).encode("utf-8")
    return struct.pack(">I", len(payload)) + payload


def _recv_exactly(conn: socket.socket, n: int) -> bytes:
    """Read exactly `n` bytes from `conn`, looping as needed."""
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed before all bytes received")
        data += chunk
    return data


def _start_mock_server(response: dict) -> tuple[int, threading.Thread]:
    """Start a one-shot mock TCP server that returns `response` to the first client."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    ready = threading.Event()

    def serve():
        ready.set()  # signal that accept() is ready
        conn, _ = server.accept()
        # Read the request (length-prefixed) — use _recv_exactly to avoid
        # partial reads that would cause struct.unpack / json.loads to fail.
        raw_len = _recv_exactly(conn, 4)
        (length,) = struct.unpack(">I", raw_len)
        _recv_exactly(conn, length)  # discard request body
        # Send the mock response
        conn.sendall(_frame(response))
        conn.close()
        server.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    ready.wait(timeout=5)
    return port, t


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestSendCommand:
    def test_successful_response(self):
        mock_resp = {
            "id": "abc123",
            "success": True,
            "stdout_output": "Hello, world!",
            "stderr_output": "",
            "exit_code": 0,
            "error_message": "",
        }
        port, t = _start_mock_server(mock_resp)

        result = send_command("127.0.0.1", port, "echo Hello, world!")
        t.join(timeout=5)

        assert result["success"] is True
        assert result["stdout_output"] == "Hello, world!"
        assert result["exit_code"] == 0

    def test_failed_response(self):
        mock_resp = {
            "id": "xyz",
            "success": False,
            "stdout_output": "",
            "stderr_output": "",
            "exit_code": 1,
            "error_message": "Command not found",
        }
        port, t = _start_mock_server(mock_resp)

        result = send_command("127.0.0.1", port, "nonexistent_command")
        t.join(timeout=5)

        assert result["success"] is False
        assert result["error_message"] == "Command not found"

    def test_service_unavailable(self):
        # Use a port that is definitely not listening
        with pytest.raises(ServiceUnavailableError):
            send_command("127.0.0.1", 1, "echo hi", connect_timeout=1.0)

    def test_request_contains_command(self):
        """Verify the request sent to the service includes the command fields."""
        received_request: list[dict] = []

        def serve(server):
            conn, _ = server.accept()
            raw_len = _recv_exactly(conn, 4)
            (length,) = struct.unpack(">I", raw_len)
            body = _recv_exactly(conn, length)
            received_request.append(json.loads(body))
            mock = {
                "id": "x", "success": True,
                "stdout_output": "ok", "stderr_output": "",
                "exit_code": 0, "error_message": "",
            }
            conn.sendall(_frame(mock))
            conn.close()
            server.close()

        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)  # listen() before thread start — connect() will succeed immediately
        port = srv.getsockname()[1]
        t = threading.Thread(target=serve, args=(srv,), daemon=True)
        t.start()

        send_command(
            "127.0.0.1", port, "ipconfig",
            description="查看网络适配器配置",
            working_dir="C:\\", timeout_seconds=30,
        )
        t.join(timeout=5)

        assert len(received_request) == 1
        req = received_request[0]
        assert req["command"] == "ipconfig"
        assert req["description"] == "查看网络适配器配置"
        assert req["working_dir"] == "C:\\"
        assert req["timeout_seconds"] == 30
        assert "id" in req

    def test_request_description_defaults_to_empty(self):
        """Omitting description sends an empty string (backward compatible)."""
        received_request: list[dict] = []

        def serve(server):
            conn, _ = server.accept()
            raw_len = _recv_exactly(conn, 4)
            (length,) = struct.unpack(">I", raw_len)
            body = _recv_exactly(conn, length)
            received_request.append(json.loads(body))
            mock = {
                "id": "x", "success": True,
                "stdout_output": "ok", "stderr_output": "",
                "exit_code": 0, "error_message": "",
            }
            conn.sendall(_frame(mock))
            conn.close()
            server.close()

        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        t = threading.Thread(target=serve, args=(srv,), daemon=True)
        t.start()

        send_command("127.0.0.1", port, "echo hi")
        t.join(timeout=5)

        assert received_request[0]["description"] == ""
