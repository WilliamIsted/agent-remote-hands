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

"""Synchronous Python client for the v2.2 wire protocol.

The conformance suite uses this to exercise verbs against a running agent.
v2.2 retired the ARH header-line wire format as an ongoing framing — it is
retained only for the `connection.hello` bootstrap. After the hello OK body
is consumed, the connection switches to MCP-stdio (default) or RFC 6455
binary frames carrying MCP JSON-RPC 2.0.

`WireClient` keeps the same public surface as the v2.1 client (`request()`,
`info()`, `capabilities()`, `tier_raise()`, …) but speaks MCP under the hood
once `hello()` returns. `WsWireClient` is a subclass that swaps the MCP-stdio
codec for an RFC 6455 binary-frame codec.

No third-party dependencies — stdlib `socket` only.
"""

from __future__ import annotations

import json
import os
import secrets
import socket
import struct
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Union


class WireError(Exception):
    """Raised on protocol-level failure (connection close mid-frame, malformed
    response). Verb-level errors are NOT exceptions; tests inspect the
    returned `OkResponse` / `ErrResponse`."""


def _quote(arg: str) -> str:
    """Wrap `arg` in ASCII double quotes if it contains a space or is empty,
    per PROTOCOL.md §1.2.5. Used by the bootstrap line only."""
    if '"' in arg:
        raise WireError(
            f"arg contains a literal double quote, which the wire format "
            f"cannot represent on the header line: {arg!r}")
    if arg == "" or " " in arg:
        return f'"{arg}"'
    return arg


def _tokenize(line: str) -> list:
    """Parse a wire header argument string into a list of string tokens.

    Inverse of ``" ".join(_quote(a) for a in args)``. Quoted tokens (wrapped
    in ``"..."`` ) may contain spaces and backslashes — backslash is a literal
    byte with no escape meaning. Unquoted tokens end at whitespace. Multiple
    consecutive spaces collapse (produce no empty tokens between them).

    Raises ``WireError`` on an unclosed quote.
    """
    tokens = []
    i = 0
    n = len(line)
    while i < n:
        while i < n and line[i] == " ":
            i += 1
        if i >= n:
            break
        if line[i] == '"':
            i += 1
            start = i
            while i < n and line[i] != '"':
                i += 1
            if i >= n:
                raise WireError("unmatched quote in wire header line")
            tokens.append(line[start:i])
            i += 1
        else:
            start = i
            while i < n and line[i] != " ":
                i += 1
            tokens.append(line[start:i])
    return tokens


@dataclass
class OkResponse:
    """Mirror of v2.1's OkResponse. `payload` carries the verb's OK-body JSON
    bytes — extracted from the MCP `tools/call` text content item so existing
    tests that `json.loads(r.payload)` continue to work unchanged."""
    payload: bytes


@dataclass
class ErrResponse:
    code: str
    detail: dict


Response = Union[OkResponse, ErrResponse]


# ---------------------------------------------------------------------------
# Argument coercion
#
# Existing tests call e.g. `client.request("file.write", "C:\\x.txt",
# "--encoding", "binary", payload=b"...")`. v2.2 MCP needs those flat args
# turned into a `params.arguments` JSON object. The mapping rules are simple
# and match the way the conformance suite has historically invoked verbs:
#
# - `--key value`          -> {"key": value}     (with `_` for `-`)
# - `--flag` (no value)    -> {"flag": true}
# - bare positional values are added to a `_args` list (rare; only legacy
#   tier_raise-style calls hit this path)
# - `payload=b"..."` is encoded as base64 and stored under
#   `content_b64` so verbs that take payloads (file.write, clipboard.set)
#   continue to receive their bytes.

def _args_to_dict(args: tuple, payload: bytes = b"") -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    positional: List[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if isinstance(a, str) and a.startswith("--"):
            key = a[2:].replace("-", "_")
            # Lookahead: if next arg exists and isn't another flag, treat as value.
            if i + 1 < len(args) and not (
                    isinstance(args[i + 1], str) and args[i + 1].startswith("--")):
                out[key] = args[i + 1]
                i += 2
            else:
                out[key] = True
                i += 1
        else:
            positional.append(a)
            i += 1
    if positional:
        out["_args"] = positional
    if payload:
        import base64
        out["content_b64"] = base64.b64encode(payload).decode("ascii")
    return out


# ---------------------------------------------------------------------------
# WireClient — bootstrap text-line, then MCP-stdio framing


class WireClient:
    """One TCP connection to the agent. Use as a context manager.

    Lifecycle:
        c = WireClient(host, port)
        c.hello()              # bootstrap text-line; auto-runs MCP initialize
        c.request("system.info")  # MCP tools/call internally
        c.close()
    """

    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = socket.create_connection(
            (host, port), timeout=timeout)
        self._buf = bytearray()
        self._next_id = 1
        self._initialized = False
        self.notifications: List[dict] = []  # buffered MCP notifications
        self.hello_body: dict = {}
        self.server_info: dict = {}
        self._tools_cache: Optional[List[dict]] = None

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
    # Bootstrap framing primitives (text-line, §1.2)

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

    def _send_line(self, header: str, payload: bytes = b"") -> None:
        assert self._sock is not None
        self._sock.sendall(header.encode("utf-8") + b"\n" + payload)

    def _read_bootstrap_response(self) -> Response:
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
        raise WireError(f"unexpected bootstrap response: {line!r}")

    # ------------------------------------------------------------------
    # MCP-stdio framing primitives (§1.6)

    def _send_mcp(self, obj: dict) -> None:
        """Encode an MCP JSON-RPC object and send it. Subclasses override
        for alternate transports (e.g. WS)."""
        assert self._sock is not None
        body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self._sock.sendall(header + body)

    def _read_mcp_frame(self) -> dict:
        """Read one MCP-stdio frame and return the parsed JSON. Subclasses
        override for alternate transports."""
        # Read header: lines terminated by \r\n, ended by an empty line.
        headers: Dict[str, str] = {}
        while True:
            line = self._read_line_crlf()
            if line == b"":
                break
            if b":" not in line:
                raise WireError(f"malformed MCP header line: {line!r}")
            k, _, v = line.partition(b":")
            headers[k.decode("ascii").strip().lower()] = v.decode("ascii").strip()
        if "content-length" not in headers:
            raise WireError("MCP frame missing Content-Length")
        n = int(headers["content-length"])
        body = self._read_bytes(n) if n > 0 else b""
        try:
            return json.loads(body)
        except json.JSONDecodeError as exc:
            raise WireError(f"malformed MCP body: {exc}") from exc

    def _read_line_crlf(self) -> bytes:
        """Read a CRLF-terminated header line, stripping the trailing \\r\\n."""
        assert self._sock is not None
        while b"\r\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise WireError("connection closed mid-MCP-header")
            self._buf.extend(chunk)
        idx = self._buf.index(b"\r\n")
        line = bytes(self._buf[:idx])
        del self._buf[: idx + 2]
        return line

    # ------------------------------------------------------------------
    # MCP request / response

    def _alloc_id(self) -> int:
        i = self._next_id
        self._next_id += 1
        return i

    def _read_mcp_response(self, expected_id: int) -> dict:
        """Read MCP frames until one matches `expected_id`. Notifications and
        unrelated requests are buffered."""
        while True:
            frame = self._read_mcp_frame()
            if frame.get("id") == expected_id:
                return frame
            if "method" in frame and "id" not in frame:
                # Notification — buffer for tests that want it.
                self.notifications.append(frame)
                continue
            # Unmatched ID or unexpected shape — buffer in notifications for
            # diagnostic visibility, then continue.
            self.notifications.append(frame)

    # ------------------------------------------------------------------
    # Public API

    def hello(self,
              client_name: str = "conformance",
              version: str = "2.2",
              framing: Optional[str] = None) -> dict:
        """Run the bootstrap hello, switch to MCP framing, run `initialize`.

        Returns the parsed hello OK body as a dict (callers that just want
        the side-effects can ignore it). When `framing == "ws"` the caller
        should be using `WsWireClient` directly — this base class only knows
        how to drive MCP-stdio post-bootstrap."""
        # Bootstrap line.
        args = [client_name, version]
        if framing:
            args.extend(["--framing", framing])
        header = "connection.hello " + " ".join(_quote(a) for a in args)
        self._send_line(header)
        r = self._read_bootstrap_response()
        if isinstance(r, ErrResponse):
            raise WireError(f"hello failed: {r.code} {r.detail}")
        body = json.loads(r.payload) if r.payload else {}
        self.hello_body = body
        # Run MCP initialize unless caller flagged ws (subclass owns that path).
        if framing != "ws":
            self._mcp_initialize(client_name)
        return body

    def _mcp_initialize(self, client_name: str) -> None:
        """Run the MCP three-step initialize handshake."""
        rid = self._alloc_id()
        self._send_mcp({
            "jsonrpc": "2.0", "id": rid, "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": client_name, "version": "1"},
            },
        })
        resp = self._read_mcp_response(rid)
        if "error" in resp:
            raise WireError(f"initialize failed: {resp['error']}")
        self.server_info = resp.get("result", {}).get("serverInfo", {})
        self._send_mcp({
            "jsonrpc": "2.0", "method": "notifications/initialized",
        })
        self._initialized = True

    def request(self, verb: str, *args: str, payload: bytes = b"") -> Response:
        """Invoke an ARH verb via MCP `tools/call`. Returns OkResponse with
        the verb's OK-body JSON bytes (so existing tests that
        `json.loads(r.payload)` keep working) or ErrResponse with the
        ARH error code and detail dict."""
        if not self._initialized:
            raise WireError("hello() must be called before request()")
        rid = self._alloc_id()
        self._send_mcp({
            "jsonrpc": "2.0", "id": rid, "method": "tools/call",
            "params": {
                "name": verb,
                "arguments": _args_to_dict(args, payload=payload),
            },
        })
        resp = self._read_mcp_response(rid)
        if "error" in resp:
            # MCP protocol-level error — surface as a WireError; verb-level
            # failures travel as isError:true results.
            err = resp["error"]
            raise WireError(
                f"MCP protocol error on {verb}: {err.get('code')} "
                f"{err.get('message')!r}")
        result = resp.get("result", {})
        text = ""
        for item in result.get("content", []):
            if item.get("type") == "text":
                text = item.get("text", "")
                break
        body = text.encode("utf-8") if text else b""
        if result.get("isError"):
            try:
                detail = json.loads(text) if text else {}
            except json.JSONDecodeError:
                detail = {"message": text}
            return ErrResponse(
                code=result.get("arh_error_code", ""),
                detail=detail,
            )
        return OkResponse(payload=body)

    def list_tools(self) -> List[dict]:
        """Return the parsed tool list from MCP `tools/list`. Cached."""
        if self._tools_cache is None:
            rid = self._alloc_id()
            self._send_mcp({
                "jsonrpc": "2.0", "id": rid, "method": "tools/list",
                "params": {},
            })
            resp = self._read_mcp_response(rid)
            if "error" in resp:
                raise WireError(f"tools/list failed: {resp['error']}")
            self._tools_cache = list(resp.get("result", {}).get("tools", []))
        return self._tools_cache

    # ------------------------------------------------------------------
    # Convenience wrappers (mirror the v2.1 surface)

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
        return self.request("connection.tier_raise",
                            "--tier", tier, "--token", token)


# ---------------------------------------------------------------------------
# WsWireClient — RFC 6455 binary frames carrying MCP JSON-RPC 2.0
#
# Bootstrap is identical (text-line `connection.hello ... --framing ws`).
# After the hello OK body is consumed, every subsequent message is wrapped
# in one RFC 6455 binary frame. ~60 lines of stdlib socket code; no
# third-party WS library needed.


class WsWireClient(WireClient):
    """WireClient subclass that uses RFC 6455 framing post-hello.

    Override surface is small: `_send_mcp` and `_read_mcp_frame` swap the
    MCP-stdio codec for a WebSocket binary-frame codec. Everything else
    (request(), info(), tools/list, etc.) is inherited unchanged."""

    # Frame field constants (RFC 6455 §5.2).
    _FIN = 0x80
    _OP_BINARY = 0x02
    _OP_PING = 0x09
    _OP_PONG = 0x0A
    _OP_CLOSE = 0x08
    _MASK = 0x80

    def hello(self,
              client_name: str = "conformance",
              version: str = "2.2",
              framing: Optional[str] = None) -> dict:
        # Force ws in the bootstrap line; ignore caller override.
        body = super().hello(client_name=client_name, version=version,
                             framing="ws")
        # Run MCP initialize over WS (parent skipped it because framing == ws).
        self._mcp_initialize(client_name)
        return body

    # ------------------------------------------------------------------
    # WS framing codec

    def _send_mcp(self, obj: dict) -> None:
        assert self._sock is not None
        payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        self._send_ws_frame(self._OP_BINARY, payload)

    def _send_ws_frame(self, opcode: int, payload: bytes) -> None:
        assert self._sock is not None
        b1 = self._FIN | (opcode & 0x0F)
        plen = len(payload)
        mask_key = secrets.token_bytes(4)
        if plen < 126:
            b2 = self._MASK | plen
            header = bytes([b1, b2]) + mask_key
        elif plen < (1 << 16):
            b2 = self._MASK | 126
            header = bytes([b1, b2]) + struct.pack("!H", plen) + mask_key
        else:
            b2 = self._MASK | 127
            header = bytes([b1, b2]) + struct.pack("!Q", plen) + mask_key
        masked = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(header + masked)

    def _read_mcp_frame(self) -> dict:
        # Read frames, ignoring/responding to control frames, until a binary
        # data frame arrives.
        while True:
            opcode, payload = self._read_ws_frame()
            if opcode == self._OP_BINARY:
                try:
                    return json.loads(payload)
                except json.JSONDecodeError as exc:
                    raise WireError(f"malformed WS body: {exc}") from exc
            if opcode == self._OP_PING:
                self._send_ws_frame(self._OP_PONG, payload)
                continue
            if opcode == self._OP_PONG:
                continue
            if opcode == self._OP_CLOSE:
                raise WireError("server sent WS close frame")
            raise WireError(f"unexpected WS opcode: {opcode:#x}")

    def _read_ws_frame(self) -> tuple:
        b = self._read_bytes(2)
        b1, b2 = b[0], b[1]
        fin = bool(b1 & self._FIN)
        opcode = b1 & 0x0F
        masked = bool(b2 & self._MASK)
        plen = b2 & 0x7F
        if plen == 126:
            plen = struct.unpack("!H", self._read_bytes(2))[0]
        elif plen == 127:
            plen = struct.unpack("!Q", self._read_bytes(8))[0]
        mask_key = self._read_bytes(4) if masked else None
        payload = self._read_bytes(plen) if plen > 0 else b""
        if mask_key is not None:
            payload = bytes(c ^ mask_key[i % 4] for i, c in enumerate(payload))
        if not fin:
            # Continuation frames not used by ARH (every message fits in one
            # frame). Treat as a hard error so the test suite catches it.
            raise WireError("WS continuation frames are not supported")
        return opcode, payload
