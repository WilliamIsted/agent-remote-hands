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

"""In-process mock of the agent's v2.2 wire protocol.

Two-phase framing — matches the canonical reference at
``tests/conformance/wire.py``:

1. **Bootstrap** (text-line, §1.2). Reads one line of the form
   ``connection.hello <client_name> <version> [--framing ws]``, sends
   ``OK 0\\n`` (or ``OK <len>\\n<json>``). After hello the connection
   switches to MCP-stdio for the rest of its life.
2. **MCP-stdio** (§1.6). ``Content-Length: N\\r\\n\\r\\n<body>`` frames
   carrying JSON-RPC 2.0. Handles ``initialize`` /
   ``notifications/initialized`` / ``tools/call``. Verb-level dispatch
   happens inside ``tools/call`` by ``params.name``.

Tests register handlers via ``MockAgent.register(verb, fn)``. The
handler signature is ``(arguments: dict, payload: bytes) -> (kind, body)``
where ``arguments`` is the MCP ``params.arguments`` object (already
parsed from JSON; ``content_b64`` decoded into ``payload`` for verbs
that carry one). ``kind`` is ``"ok"`` or an ARH error code; ``body`` is
a JSON-serialisable dict (or ``None`` for empty OK).

Concurrency-stress hook: pass ``release_barrier_n=N`` to ``MockAgent``
to make each ``tools/call`` dispatch wait at a ``threading.Barrier(N)``
before sending its response. With ``N`` set to the number of concurrent
caller threads, all responses are released in one burst — deterministically
widening the response-side window for client-side framing-race tests.
"""

from __future__ import annotations

import base64
import json
import socket
import threading
from typing import Any, Callable, Dict, Optional, Tuple

# Type alias for clarity. Handler returns (kind, body); kind is "ok" or an
# ARH error code; body is JSON-serialisable or None.
Handler = Callable[[Dict[str, Any], bytes], Tuple[str, Optional[Dict[str, Any]]]]


class MockAgent:
    """TCP server that speaks the v2.2 wire protocol. One thread per
    accepted connection. Bind a port (default 0 = ephemeral) and call
    ``start()``; ``port`` is then readable. ``stop()`` closes the listener
    and joins worker threads."""

    def __init__(
        self,
        token: str = "test-token-123",
        release_barrier_n: Optional[int] = None,
    ) -> None:
        self.token = token
        self._listen: Optional[socket.socket] = None
        self.port: int = 0
        self._serve_thread: Optional[threading.Thread] = None
        self._client_threads: list[threading.Thread] = []
        self._stopping = False
        self._handlers: Dict[str, Handler] = {}
        self._tier_per_conn: Dict[int, str] = {}

        # Concurrency-stress: tools/call dispatches wait at this barrier
        # before sending their response. None = no barrier (normal mode).
        self._release_barrier: Optional[threading.Barrier] = (
            threading.Barrier(release_barrier_n) if release_barrier_n else None
        )

    # ------------------------------------------------------------------
    # Lifecycle

    def start(self) -> None:
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.bind(("127.0.0.1", 0))
        self._listen.listen(4)
        self.port = self._listen.getsockname()[1]
        self._serve_thread = threading.Thread(
            target=self._accept_loop, daemon=True)
        self._serve_thread.start()

    def stop(self) -> None:
        self._stopping = True
        # Aborting the barrier wakes any handler threads parked there so they
        # can exit cleanly during teardown instead of hanging the test.
        if self._release_barrier is not None:
            try:
                self._release_barrier.abort()
            except threading.BrokenBarrierError:
                pass
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

    def register(self, verb: str, handler: Handler) -> None:
        """Register a handler for ``verb`` (MCP ``params.name``). The handler
        receives ``(arguments, payload)`` and returns ``(kind, body)`` where
        ``kind`` is ``"ok"`` or an ARH error code, ``body`` is a JSON-
        serialisable dict (or ``None`` for an empty OK)."""
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
        self._tier_per_conn[conn_id] = "read"
        buf = bytearray()
        try:
            # --- Bootstrap phase: one line, expect connection.hello. ---
            line = self._read_line(sock, buf)
            text = line.decode("utf-8", "replace")
            if not text.startswith("connection.hello"):
                # The conformance contract is hello-first; anything else is
                # a protocol violation, not a verb we can helpfully NACK.
                self._send_bootstrap_err(
                    sock, "invalid_args",
                    {"message": "first line must be connection.hello"})
                return
            self._send_bootstrap_ok(sock, body=None)

            # --- MCP-stdio phase: Content-Length frames forever. ---
            while not self._stopping:
                frame = self._read_mcp_frame(sock, buf)
                if frame is None:
                    return  # EOF
                self._dispatch_mcp(sock, conn_id, frame)
        except (OSError, ConnectionResetError):
            return
        finally:
            try:
                sock.close()
            except OSError:
                pass
            self._tier_per_conn.pop(conn_id, None)

    # ------------------------------------------------------------------
    # Bootstrap framing (text-line)

    @staticmethod
    def _read_line(sock: socket.socket, buf: bytearray) -> bytes:
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionResetError("EOF during bootstrap")
            buf.extend(chunk)
        idx = buf.index(b"\n")
        line = bytes(buf[:idx]).rstrip(b"\r")
        del buf[: idx + 1]
        return line

    @staticmethod
    def _send_bootstrap_ok(sock: socket.socket,
                           body: Optional[dict]) -> None:
        if body is None:
            sock.sendall(b"OK 0\n")
            return
        encoded = json.dumps(body).encode("utf-8")
        sock.sendall(f"OK {len(encoded)}\n".encode("ascii") + encoded)

    @staticmethod
    def _send_bootstrap_err(sock: socket.socket,
                            code: str, detail: dict) -> None:
        if not detail:
            sock.sendall(f"ERR {code} 0\n".encode("ascii"))
            return
        encoded = json.dumps(detail).encode("utf-8")
        sock.sendall(
            f"ERR {code} {len(encoded)}\n".encode("ascii") + encoded)

    # ------------------------------------------------------------------
    # MCP-stdio framing

    @staticmethod
    def _read_mcp_frame(sock: socket.socket,
                        buf: bytearray) -> Optional[dict]:
        """Read one MCP-stdio JSON-RPC frame from ``sock`` using ``buf`` as
        the receive buffer. Returns the parsed JSON or None on clean EOF."""
        headers: Dict[str, str] = {}
        # Read header lines until empty CRLF.
        while True:
            while b"\r\n" not in buf:
                chunk = sock.recv(4096)
                if not chunk:
                    return None
                buf.extend(chunk)
            idx = buf.index(b"\r\n")
            line = bytes(buf[:idx])
            del buf[: idx + 2]
            if line == b"":
                break
            if b":" not in line:
                raise ConnectionResetError(
                    f"malformed MCP header line: {line!r}")
            k, _, v = line.partition(b":")
            headers[k.decode("ascii").strip().lower()] = (
                v.decode("ascii").strip())
        if "content-length" not in headers:
            raise ConnectionResetError("MCP frame missing Content-Length")
        n = int(headers["content-length"])
        while len(buf) < n:
            chunk = sock.recv(max(1, n - len(buf)))
            if not chunk:
                raise ConnectionResetError("EOF mid-MCP-body")
            buf.extend(chunk)
        body = bytes(buf[:n])
        del buf[:n]
        return json.loads(body) if body else {}

    @staticmethod
    def _send_mcp(sock: socket.socket, obj: dict) -> None:
        body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        sock.sendall(header + body)

    # ------------------------------------------------------------------
    # MCP dispatch

    def _dispatch_mcp(self, sock: socket.socket, conn_id: int,
                      frame: dict) -> None:
        method = frame.get("method")
        rid = frame.get("id")

        # Notifications carry a method but no id; they require no response.
        if method == "notifications/initialized":
            return

        if method == "initialize":
            self._send_mcp(sock, {
                "jsonrpc": "2.0", "id": rid,
                "result": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "serverInfo": {
                        "name": "mock-agent",
                        "version": "0.0.0",
                    },
                },
            })
            return

        if method == "tools/call":
            params = frame.get("params", {})
            verb = params.get("name", "")
            arguments = params.get("arguments", {}) or {}
            payload = self._decode_payload(arguments)
            kind, body = self._dispatch_verb(conn_id, verb, arguments, payload)
            # Optional barrier widens the response-side window for client-
            # locking regression tests. Each handler thread parks here until
            # `release_barrier_n` requests have arrived, then all release
            # together. Built-in verbs (system.*, connection.*) bypass the
            # barrier — they're fired implicitly by `connect()` and would
            # otherwise stall the fixture setup. Only test-registered handlers
            # exercise the barrier. Aborted barriers (teardown) fall through
            # silently.
            if (self._release_barrier is not None
                    and verb in self._handlers):
                try:
                    self._release_barrier.wait(timeout=10.0)
                except threading.BrokenBarrierError:
                    pass
            self._send_tools_call_response(sock, rid, kind, body)
            return

        # Unknown JSON-RPC method — surface as a protocol-level error.
        if rid is not None:
            self._send_mcp(sock, {
                "jsonrpc": "2.0", "id": rid,
                "error": {"code": -32601,
                          "message": f"method not found: {method!r}"},
            })

    @staticmethod
    def _decode_payload(arguments: Dict[str, Any]) -> bytes:
        """Extract and decode the ``content_b64`` argument, if present.
        Verbs that carry a binary payload (file.write, clipboard.set, …)
        send it base64-encoded under this key; the rest get an empty
        bytes payload. Removes the key from ``arguments`` so handlers see
        only their verb's own args."""
        b64 = arguments.pop("content_b64", None)
        if not b64:
            return b""
        try:
            return base64.b64decode(b64, validate=True)
        except (ValueError, TypeError):
            return b""

    def _send_tools_call_response(self, sock: socket.socket,
                                  rid: Any, kind: str,
                                  body: Optional[dict]) -> None:
        """Encode a verb response into the MCP ``tools/call`` result shape
        the v2.2 bridge expects (`content` array of text/image items, optional
        ``isError`` + ``arh_error_code`` on error)."""
        text = json.dumps(body) if body else ""
        if kind == "ok":
            self._send_mcp(sock, {
                "jsonrpc": "2.0", "id": rid,
                "result": {
                    "content": [{"type": "text", "text": text}] if text else [],
                },
            })
            return
        # Error path: isError + arh_error_code, detail JSON in the text item.
        self._send_mcp(sock, {
            "jsonrpc": "2.0", "id": rid,
            "result": {
                "content": [{"type": "text", "text": text}],
                "isError": True,
                "arh_error_code": kind,
            },
        })

    # ------------------------------------------------------------------
    # Built-in verb handlers (connection.* and system.*)

    def _dispatch_verb(self, conn_id: int, verb: str,
                       arguments: Dict[str, Any],
                       payload: bytes) -> Tuple[str, Optional[dict]]:
        # ----- connection.* tier management -----
        if verb == "connection.tier_raise":
            target = arguments.get("tier", "")
            token = arguments.get("token", "")
            if token != self.token:
                return ("auth_invalid", {"message": "token mismatch"})
            order = {"read": 0, "create": 1, "update": 2,
                     "delete": 3, "extra_risky": 4}
            cur = self._tier_per_conn.get(conn_id, "read")
            if target not in order:
                return ("invalid_args",
                        {"message": f"unknown tier {target!r}"})
            if order[target] <= order.get(cur, 0):
                return ("invalid_args", {"message": "use tier_drop"})
            self._tier_per_conn[conn_id] = target
            return ("ok", {"new_tier": target})

        if verb == "connection.tier_drop":
            target = arguments.get("tier", "read")
            self._tier_per_conn[conn_id] = target
            return ("ok", {"new_tier": target})

        if verb == "connection.reset":
            return ("ok", None)

        # ----- system.* introspection -----
        if verb == "system.info":
            return ("ok", {
                "name": "mock-agent",
                "version": "0.0.0",
                "protocol": "2.1",
                "os": "windows-modern",
                "arch": "x64",
                "hostname": "mock",
                "user": "tester",
                "integrity": "medium",
                "uiaccess": False,
                "monitors": 1,
                "privileges": [],
                "tiers": ["read", "create", "update",
                          "delete", "extra_risky"],
                "current_tier": self._tier_per_conn.get(conn_id, "read"),
                "auth": ["token"],
                "max_connections": 4,
                "namespaces": ["system", "connection"],
                "capabilities": {
                    "capture": "gdi",
                    "ui_automation": "uia",
                    "image_formats": ["png", "bmp"],
                },
            })
        if verb == "system.health":
            return ("ok", None)

        # ----- custom handlers registered by tests -----
        if verb in self._handlers:
            return self._handlers[verb](arguments, payload)

        # ----- fallback -----
        return ("not_supported", {"verb": verb})
