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

"""Synchronous Python client for the v2 wire protocol.

The conformance suite uses this to exercise verbs against a running agent.
It is deliberately minimal — no async, no streaming-friendly interface,
no high-level verb wrappers beyond `hello()`, `info()`, and `capabilities()`.
Tests build the verb strings themselves so the wire surface remains visible.
"""

from __future__ import annotations

import json
import socket
from dataclasses import dataclass
from typing import Optional, Tuple, Union


class WireError(Exception):
    """Raised on protocol-level failure (connection close mid-frame, malformed
    response). Verb-level `ERR <code>` responses are NOT exceptions; tests
    inspect the returned tuple."""


def _quote(arg: str) -> str:
    """Wrap `arg` in ASCII double quotes if it contains a space or is empty,
    per PROTOCOL.md §1.2.5. Args containing a literal `"` raise WireError —
    the wire grammar has no escape mechanism inside quotes; verbs that need
    raw bytes containing `"` use the length-prefixed payload form.
    Backslashes inside the arg are literal (so Windows paths work)."""
    if '"' in arg:
        raise WireError(
            f"arg contains a literal double quote, which the wire format "
            f"cannot represent on the header line: {arg!r}")
    if arg == "" or " " in arg:
        return f'"{arg}"'
    return arg


def _tokenize(line: str) -> list[str]:
    """Split a header line into tokens per PROTOCOL.md §1.2.5. Used by the
    conformance suite to validate quoting round-trips. The agent has its own
    parser; this is the reference implementation."""
    tokens: list[str] = []
    i = 0
    n = len(line)
    while i < n:
        # Skip leading spaces
        while i < n and line[i] == " ":
            i += 1
        if i >= n:
            break
        if line[i] == '"':
            # Quoted token. Read until the next ".
            i += 1
            start = i
            while i < n and line[i] != '"':
                i += 1
            if i >= n:
                raise WireError("unmatched quote in header")
            tokens.append(line[start:i])
            i += 1  # skip the closing "
        else:
            # Unquoted token. Read until the next space.
            start = i
            while i < n and line[i] != " ":
                i += 1
            tokens.append(line[start:i])
    return tokens


@dataclass
class OkResponse:
    payload: bytes


@dataclass
class ErrResponse:
    code: str
    detail: dict


Response = Union[OkResponse, ErrResponse]


class WireClient:
    """One TCP connection to the agent. Use as a context manager."""

    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = socket.create_connection(
            (host, port), timeout=timeout)
        self._buf = bytearray()

    # ------------------------------------------------------------------
    # Lifecycle

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "WireClient":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

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

    def _read_bytes(self, n: int) -> bytes:
        assert self._sock is not None
        while len(self._buf) < n:
            chunk = self._sock.recv(min(65536, n - len(self._buf)))
            if not chunk:
                raise WireError("connection closed mid-payload")
            self._buf.extend(chunk)
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def _send(self, header: str, payload: bytes = b"") -> None:
        assert self._sock is not None
        self._sock.sendall(header.encode("utf-8") + b"\n" + payload)

    # ------------------------------------------------------------------
    # Request / response

    def request(self, verb: str, *args: str, payload: bytes = b"") -> Response:
        """Send a verb and return its response.

        Args containing spaces (or empty args) are automatically wrapped in
        ASCII double quotes per PROTOCOL.md §1.2.5. Args containing a literal
        double quote `"` are not representable on the header line and raise
        WireError before sending.

        EVENT frames received before the response are silently discarded; tests
        that care about events should call `next_event()` directly on a side
        connection."""
        header = verb if not args else verb + " " + " ".join(_quote(a) for a in args)
        self._send(header, payload)
        return self._read_response()

    def _read_response(self) -> Response:
        while True:
            line = self._read_line().decode("utf-8")
            parts = line.split(" ", 2)
            head = parts[0]
            if head == "OK":
                length = int(parts[1]) if len(parts) > 1 else 0
                body = self._read_bytes(length) if length > 0 else b""
                return OkResponse(payload=body)
            if head == "ERR":
                code = parts[1] if len(parts) > 1 else ""
                length = int(parts[2]) if len(parts) > 2 else 0
                body = self._read_bytes(length) if length > 0 else b""
                detail = json.loads(body) if body else {}
                return ErrResponse(code=code, detail=detail)
            if head == "EVENT":
                length = int(parts[2]) if len(parts) > 2 else 0
                if length > 0:
                    self._read_bytes(length)
                continue
            raise WireError(f"unexpected response: {line!r}")

    # ------------------------------------------------------------------
    # Convenience wrappers

    def hello(self, client_name: str = "conformance",
              version: str = "2.0") -> None:
        r = self.request("connection.hello", client_name, version)
        if isinstance(r, ErrResponse):
            raise WireError(f"hello failed: {r.code} {r.detail}")

    def info(self) -> dict:
        r = self.request("system.info")
        if isinstance(r, ErrResponse):
            raise WireError(f"system.info failed: {r.code}")
        return json.loads(r.payload)

    def capabilities(self) -> dict:
        r = self.request("system.capabilities")
        if isinstance(r, ErrResponse):
            raise WireError(f"system.capabilities failed: {r.code}")
        return json.loads(r.payload)

    def tier_raise(self, tier: str, token: str) -> Response:
        return self.request("connection.tier_raise", tier, token)
