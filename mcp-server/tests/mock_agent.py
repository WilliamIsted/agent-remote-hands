#   Copyright 2026 William Isted and contributors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

"""In-process mock of the agent's wire protocol.

Just enough framing fidelity to drive `agent_client.AgentClient` end-to-end
without booting a real Windows agent. Implements:

- `connection.hello` (any protocol version starting with `2.`)
- `connection.tier_raise <tier> <token>` with token check
- `connection.tier_drop <tier>`
- `connection.reset`
- `connection.close`
- `system.info` returning a stub JSON
- `system.health`
- Anything else returns `ERR not_supported` with `{verb}` detail

Tests register handlers via `MockAgent.register(verb, fn)` for verbs they
care about, so each test scope can simulate whatever response shape the
handler under test expects.
"""

from __future__ import annotations

import json
import socket
import threading
from typing import Callable, Optional


class MockAgent:
    """Simple TCP server that speaks the v2 wire protocol. One thread per
    accepted connection. Bind a port (default 0 = ephemeral) and call
    `start()`; `port` is then readable. `stop()` closes the listener and
    joins worker threads."""

    def __init__(self, token: str = "test-token-123") -> None:
        self.token = token
        self._listen: Optional[socket.socket] = None
        self.port: int = 0
        self._serve_thread: Optional[threading.Thread] = None
        self._client_threads: list[threading.Thread] = []
        self._stopping = False
        self._handlers: dict[str, Callable[[list[str], bytes], tuple[str, dict]]] = {}
        self._tier_per_conn: dict[int, str] = {}

    # ------------------------------------------------------------------
    # Lifecycle

    def start(self) -> None:
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.bind(("127.0.0.1", 0))
        self._listen.listen(4)
        self.port = self._listen.getsockname()[1]
        self._serve_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._serve_thread.start()

    def stop(self) -> None:
        self._stopping = True
        if self._listen is not None:
            try:
                self._listen.close()
            except OSError:
                pass
            self._listen = None
        for t in list(self._client_threads):
            t.join(timeout=1.0)

    # ------------------------------------------------------------------
    # Handler registration (per-test customisation)

    def register(self, verb: str,
                 handler: Callable[[list[str], bytes], tuple[str, dict]]) -> None:
        """Register a handler for `verb`. The handler receives (args, payload)
        and returns (kind, body) where kind is 'ok' or an error code, body is
        a dict serialised as JSON. For 'ok', body may be None to send `OK 0`."""
        self._handlers[verb] = handler

    # ------------------------------------------------------------------
    # Internal accept + per-connection serve

    def _accept_loop(self) -> None:
        assert self._listen is not None
        while not self._stopping:
            try:
                client, _addr = self._listen.accept()
            except OSError:
                return
            t = threading.Thread(target=self._serve_client,
                                 args=(client,), daemon=True)
            t.start()
            self._client_threads.append(t)

    def _serve_client(self, sock: socket.socket) -> None:
        conn_id = id(sock)
        self._tier_per_conn[conn_id] = "observe"
        buf = bytearray()
        try:
            while True:
                while b"\n" not in buf:
                    chunk = sock.recv(4096)
                    if not chunk:
                        return
                    buf.extend(chunk)
                idx = buf.index(b"\n")
                line = bytes(buf[:idx]).rstrip(b"\r")
                del buf[: idx + 1]

                self._dispatch(sock, conn_id, line, buf)
        except (OSError, ConnectionResetError):
            return
        finally:
            try:
                sock.close()
            except OSError:
                pass
            self._tier_per_conn.pop(conn_id, None)

    def _dispatch(self, sock: socket.socket, conn_id: int,
                  line: bytes, buf: bytearray) -> None:
        text = line.decode("utf-8", "replace")
        parts = text.split(" ")
        verb = parts[0]
        args = parts[1:]

        # Strip trailing length on header (last numeric field is body length
        # for verbs like file.write etc.). For our stub we don't need it.
        payload = b""

        # Built-in verbs.
        if verb == "connection.hello":
            self._send_ok(sock)
            return
        if verb == "connection.tier_raise":
            target = args[0] if args else ""
            token = args[1] if len(args) > 1 else ""
            if token != self.token:
                self._send_err(sock, "auth_invalid", {"message": "token mismatch"})
                return
            order = {"observe": 0, "drive": 1, "power": 2}
            cur = self._tier_per_conn.get(conn_id, "observe")
            if order.get(target, 99) <= order.get(cur, 0):
                self._send_err(sock, "invalid_args", {"message": "use tier_drop"})
                return
            self._tier_per_conn[conn_id] = target
            self._send_ok(sock, {"new_tier": target})
            return
        if verb == "connection.tier_drop":
            target = args[0] if args else "observe"
            self._tier_per_conn[conn_id] = target
            self._send_ok(sock, {"new_tier": target})
            return
        if verb == "connection.reset":
            buf.clear()
            self._send_ok(sock)
            return
        if verb == "connection.close":
            self._send_ok(sock)
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        if verb == "system.info":
            self._send_ok(sock, {
                "name": "mock-agent",
                "version": "0.0.0",
                "protocol": "2.0",
                "os": "windows-modern",
                "arch": "x64",
                "hostname": "mock",
                "user": "tester",
                "integrity": "medium",
                "uiaccess": False,
                "monitors": 1,
                "privileges": [],
                "tiers": ["observe", "drive", "power"],
                "current_tier": self._tier_per_conn.get(conn_id, "observe"),
                "auth": ["token"],
                "max_connections": 4,
                "namespaces": ["system", "connection"],
                "capabilities": {
                    "capture": "gdi",
                    "ui_automation": "uia",
                    "image_formats": ["png", "bmp"],
                },
            })
            return
        if verb == "system.health":
            self._send_ok(sock)
            return

        # Custom handler if registered.
        if verb in self._handlers:
            kind, body = self._handlers[verb](args, payload)
            if kind == "ok":
                self._send_ok(sock, body)
            else:
                self._send_err(sock, kind, body or {})
            return

        # Default: not supported.
        self._send_err(sock, "not_supported", {"verb": verb})

    def _send_ok(self, sock: socket.socket, body: Optional[dict] = None) -> None:
        if body is None:
            sock.sendall(b"OK 0\n")
            return
        encoded = json.dumps(body).encode("utf-8")
        sock.sendall(f"OK {len(encoded)}\n".encode() + encoded)

    def _send_err(self, sock: socket.socket, code: str, detail: dict) -> None:
        if not detail:
            sock.sendall(f"ERR {code} 0\n".encode())
            return
        encoded = json.dumps(detail).encode("utf-8")
        sock.sendall(f"ERR {code} {len(encoded)}\n".encode() + encoded)
