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

"""Wire-protocol client for the Agent Remote Hands TCP service.

This is a self-contained re-implementation of the v2 wire protocol — kept in
sync with `tests/conformance/wire.py` (the canonical reference). Both files
implement the same framing rules; if you change one, change the other.

The client is **single-connection, single-thread** by design. The MCP server
holds one `AgentClient` for the lifetime of the MCP session, mutates its tier
as elevation requests happen, and serialises tool calls through it. That
matches typical LLM-driven flows (one tool at a time) and avoids dealing with
per-call connection setup overhead.
"""

from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Union


# ---------------------------------------------------------------------------
# Wire types

@dataclass
class OkResponse:
    payload: bytes

    def json(self) -> dict:
        return json.loads(self.payload) if self.payload else {}


@dataclass
class ErrResponse:
    code: str
    detail: dict


Response = Union[OkResponse, ErrResponse]


class WireError(RuntimeError):
    """Raised on framing-level failures (socket dead, oversized header, etc)."""


# ---------------------------------------------------------------------------
# Token file

def read_token_file(path: Optional[str] = None) -> Optional[str]:
    """Reads the agent's elevation token. Returns None if the file isn't
    present or readable — callers should treat that as "auth disabled" and
    not attempt elevation.

    Default path matches `agents/windows-modern/src/config.cpp`'s
    `default_token_path()`.
    """
    if path is None:
        path = r"C:\ProgramData\AgentRemoteHands\token"
    p = Path(path)
    if not p.exists():
        return None
    try:
        return p.read_text(encoding="ascii").strip() or None
    except OSError:
        return None


# ---------------------------------------------------------------------------
# AgentClient

class AgentClient:
    """One TCP connection to a running agent. Tier state is held here and
    mutated by `tier_raise` / `tier_drop`.

    Usage:
        client = AgentClient("127.0.0.1", 8765)
        client.connect()
        info = client.info()
        # ...
        client.close()
    """

    PROTOCOL_VERSION = "2.0"

    def __init__(
        self,
        host: str,
        port: int,
        client_name: str = "mcp-server",
        timeout: float = 30.0,
    ) -> None:
        self.host = host
        self.port = port
        self.client_name = client_name
        self.timeout = timeout

        self._sock: Optional[socket.socket] = None
        self._buf: bytearray = bytearray()
        self._lock = threading.Lock()  # serialises requests across threads

        self._current_tier: str = "observe"
        self._token: Optional[str] = None  # cached after first read

    # ------------------------------------------------------------------
    # Lifecycle

    def connect(self) -> None:
        """Opens the socket and performs `connection.hello`. Idempotent
        only in the sense that re-calling on a live connection re-hellos
        — most callers should `close()` first if they want to reset."""
        if self._sock is not None:
            return
        self._sock = socket.create_connection(
            (self.host, self.port), timeout=self.timeout)
        r = self.request("connection.hello", self.client_name, self.PROTOCOL_VERSION)
        if isinstance(r, ErrResponse):
            self.close()
            raise WireError(f"hello failed: {r.code} {r.detail}")
        # Track the actual tier reported by the agent rather than our default.
        try:
            info = self.info()
            self._current_tier = info.get("current_tier", "observe")
        except Exception:
            pass  # If info fails for any reason, observe is a safe default.

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None
                self._buf.clear()

    def __enter__(self) -> "AgentClient":
        self.connect()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Tier state (read-only externally)

    @property
    def current_tier(self) -> str:
        return self._current_tier

    def can_satisfy(self, required: str) -> bool:
        """True if `required` is at or below the current tier."""
        order = {"observe": 0, "drive": 1, "power": 2}
        return order.get(self._current_tier, 0) >= order.get(required, 99)

    def set_token(self, token: str) -> None:
        self._token = token

    # ------------------------------------------------------------------
    # Tier transitions

    def tier_raise(self, target: str, token: Optional[str] = None) -> Response:
        """Raise to `target` tier. If `token` is None, uses the cached token
        (set via `set_token` or read from the token file). On success
        updates `current_tier`."""
        tok = token or self._token
        if tok is None:
            return ErrResponse("auth_invalid", {"message": "no token available"})
        r = self.request("connection.tier_raise", target, tok)
        if isinstance(r, OkResponse):
            try:
                self._current_tier = r.json().get("new_tier", target)
            except Exception:
                self._current_tier = target
        return r

    def tier_drop(self, target: str = "observe") -> Response:
        r = self.request("connection.tier_drop", target)
        if isinstance(r, OkResponse):
            try:
                self._current_tier = r.json().get("new_tier", target)
            except Exception:
                self._current_tier = target
        return r

    # ------------------------------------------------------------------
    # Convenience accessors

    def info(self) -> dict:
        r = self.request("system.info")
        if isinstance(r, ErrResponse):
            raise WireError(f"system.info failed: {r.code} {r.detail}")
        return r.json()

    def capabilities(self) -> dict:
        r = self.request("system.capabilities")
        if isinstance(r, ErrResponse):
            raise WireError(f"system.capabilities failed: {r.code} {r.detail}")
        return r.json()

    # ------------------------------------------------------------------
    # Generic request

    def request(self, verb: str, *args: str, payload: bytes = b"") -> Response:
        """Sends a verb with optional args and length-prefixed payload, then
        reads exactly one OK / ERR response. Thread-safe."""
        with self._lock:
            if self._sock is None:
                raise WireError("not connected")

            header = " ".join((verb, *args))
            if payload:
                header = f"{header} {len(payload)}"
            line = header.encode("utf-8") + b"\n"
            self._sock.sendall(line)
            if payload:
                self._sock.sendall(payload)

            return self._read_response()

    def _read_response(self) -> Response:
        line = self._read_line()
        parts = line.split(b" ", 2)
        directive = parts[0]
        if directive == b"OK":
            length = 0
            if len(parts) >= 2:
                length = int(parts[1])
            body = self._read_exact(length) if length else b""
            return OkResponse(body)
        elif directive == b"ERR":
            if len(parts) < 2:
                return ErrResponse("malformed", {"line": line.decode("utf-8", "replace")})
            code = parts[1].decode("ascii", "replace")
            detail: dict = {}
            if len(parts) >= 3:
                length = int(parts[2])
                if length:
                    body = self._read_exact(length)
                    try:
                        detail = json.loads(body)
                    except json.JSONDecodeError:
                        detail = {"raw": body.decode("utf-8", "replace")}
            return ErrResponse(code, detail)
        elif directive == b"EVENT":
            # Drain and ignore — the MCP server doesn't subscribe yet. If
            # subscriptions land later, this needs a queue.
            if len(parts) >= 3:
                length = int(parts[2])
                if length:
                    self._read_exact(length)
            return self._read_response()
        raise WireError(f"unknown directive: {directive!r}")

    # ------------------------------------------------------------------
    # Framing primitives

    def _read_line(self) -> bytes:
        assert self._sock is not None
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise WireError("connection closed mid-line")
            self._buf.extend(chunk)
        idx = self._buf.index(b"\n")
        line = bytes(self._buf[:idx])
        del self._buf[: idx + 1]
        if line.endswith(b"\r"):
            line = line[:-1]
        return line

    def _read_exact(self, n: int) -> bytes:
        assert self._sock is not None
        while len(self._buf) < n:
            chunk = self._sock.recv(max(4096, n - len(self._buf)))
            if not chunk:
                raise WireError("connection closed mid-payload")
            self._buf.extend(chunk)
        body = bytes(self._buf[:n])
        del self._buf[:n]
        return body
