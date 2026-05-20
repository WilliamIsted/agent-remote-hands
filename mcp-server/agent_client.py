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

"""Wire-protocol client for the v2.2 Agent Remote Hands TCP service.

This file is a HARD COPY of ``tests/conformance/wire.py`` (the canonical
v2.2 reference client), plus an ``AgentClient`` adapter that gives the MCP
bridge the same connect / tier / info / capabilities / request surface that
the v2.1 client offered. The bridge ships standalone (separate from the
conformance suite), so duplicating the wire code is the simpler choice over
import-path gymnastics — at the cost of needing a sync pass whenever
wire.py changes. The base classes ``WireClient`` / ``WsWireClient`` and the
helpers ``_quote`` / ``_tokenize`` / ``_args_to_dict`` MUST be kept
character-identical to ``tests/conformance/wire.py``; the AgentClient layer
below the divider is bridge-only.

v2.2 framing summary (see ``PROTOCOL.md`` §1 and ``spec/framing/``):

1. A new TCP connection sends ``connection.hello <client> <version>\\n`` in
   the legacy ARH header-line format.
2. The agent replies ``OK <len>\\n<json>`` with framings + serverInfo.
3. From then on, every frame is an MCP JSON-RPC 2.0 message wrapped in a
   ``Content-Length: <N>\\r\\n\\r\\n<body>`` block. Verb invocations travel
   as ``tools/call``; verb results travel as ``result.content[0].text``
   with the verb's OK body verbatim. Verb-level errors travel as
   ``result.isError: true`` with ``result.arh_error_code`` and a JSON
   error-detail body in the same text field.
4. Subscription events arrive as ``notifications/arh/event`` JSON-RPC
   notifications interleaved with normal request/response pairs.
"""

from __future__ import annotations

import base64
import json
import os
import secrets
import socket
import struct
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Union


# ===========================================================================
# Verbatim from tests/conformance/wire.py
# ===========================================================================


class WireError(Exception):
    """Raised on protocol-level failure (connection close mid-frame, malformed
    response). Verb-level errors are NOT exceptions; callers inspect the
    returned ``OkResponse`` / ``ErrResponse``."""


def _quote(arg: str) -> str:
    """Wrap ``arg`` in ASCII double quotes if it contains a space or is empty,
    per PROTOCOL.md §1.2.5. Used by the bootstrap line only."""
    if '"' in arg:
        raise WireError(
            f"arg contains a literal double quote, which the wire format "
            f"cannot represent on the header line: {arg!r}")
    if arg == "" or " " in arg:
        return f'"{arg}"'
    return arg


def _tokenize(line: str) -> list:
    """Parse a wire header argument string into a list of string tokens."""
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
    """Mirror of v2.1's OkResponse. ``payload`` carries the verb's OK-body
    JSON bytes — extracted from the MCP ``tools/call`` text content item so
    existing callers that ``json.loads(r.payload)`` continue to work
    unchanged."""
    payload: bytes

    def json(self) -> dict:
        """Bridge-side convenience — handlers want to peek at the OK body
        dict without re-parsing manually."""
        return json.loads(self.payload) if self.payload else {}


@dataclass
class ErrResponse:
    code: str
    detail: dict


Response = Union[OkResponse, ErrResponse]


def _args_to_dict(args: tuple, payload: bytes = b"") -> Dict[str, Any]:
    """Convert v2.1-style positional + flag args into a v2.2 MCP arguments
    object. See PROTOCOL.md §1.6 and wire.py source comments for the
    full mapping rules."""
    out: Dict[str, Any] = {}
    positional: List[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if isinstance(a, str) and a.startswith("--"):
            key = a[2:].replace("-", "_")
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
        out["content_b64"] = base64.b64encode(payload).decode("ascii")
    return out


class WireClient:
    """One TCP connection to the agent (v2.2 framing).

    Use as a context manager OR call ``hello()`` / ``close()`` explicitly.
    After ``hello()`` returns, ``request(verb, ...)`` invokes verbs over
    the MCP-stdio framing layer.
    """

    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = socket.create_connection(
            (host, port), timeout=timeout)
        self._buf = bytearray()
        self._next_id = 1
        self._initialized = False
        self.notifications: List[dict] = []
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
    # Bootstrap framing primitives

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
    # MCP-stdio framing primitives

    def _send_mcp(self, obj: dict) -> None:
        assert self._sock is not None
        body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self._sock.sendall(header + body)

    def _read_mcp_frame(self) -> dict:
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
        while True:
            frame = self._read_mcp_frame()
            if frame.get("id") == expected_id:
                return frame
            if "method" in frame and "id" not in frame:
                self.notifications.append(frame)
                continue
            self.notifications.append(frame)

    # ------------------------------------------------------------------
    # Public API

    def hello(self,
              client_name: str = "conformance",
              version: str = "2.2",
              framing: Optional[str] = None) -> dict:
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
        if framing != "ws":
            self._mcp_initialize(client_name)
        return body

    def _mcp_initialize(self, client_name: str) -> None:
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
        """Invoke an ARH verb. v2.1-style positional + flag args; payload
        becomes ``content_b64`` in the MCP arguments object. For callers
        that already have a schema-correct arguments dict (e.g. nested
        ``region`` object), use :meth:`request_args` instead."""
        return self.request_args(
            verb, _args_to_dict(args, payload=payload))

    def request_args(self, verb: str, arguments: Dict[str, Any]) -> Response:
        """Invoke an ARH verb with a schema-correct arguments dict. Use this
        when the verb's input_schema has nested objects, arrays, or other
        shapes that don't survive the positional-and-flags round-trip
        (e.g. ``screen.capture`` ``region: {x,y,w,h}``)."""
        if not self._initialized:
            raise WireError("hello() must be called before request()")
        rid = self._alloc_id()
        self._send_mcp({
            "jsonrpc": "2.0", "id": rid, "method": "tools/call",
            "params": {
                "name": verb,
                "arguments": arguments,
            },
        })
        resp = self._read_mcp_response(rid)
        if "error" in resp:
            err = resp["error"]
            raise WireError(
                f"MCP protocol error on {verb}: {err.get('code')} "
                f"{err.get('message')!r}")
        result = resp.get("result", {})
        text = ""
        image_bytes: Optional[bytes] = None
        for item in result.get("content", []):
            t = item.get("type")
            if t == "text" and not text:
                text = item.get("text", "")
            elif t == "image" and image_bytes is None:
                # MCP image content item — screen.capture / similar return
                # `{"type":"image","data":"<base64>","mimeType":"<mime>"}`.
                # The verb's contract on this bridge is "OkResponse.payload =
                # raw image bytes" (the v2.1 tools.py handlers base64-encode
                # for the LLM-facing response), so decode here.
                data = item.get("data", "")
                if isinstance(data, str):
                    try:
                        image_bytes = base64.b64decode(data, validate=True)
                    except (ValueError, TypeError):
                        image_bytes = data.encode("utf-8")
                else:
                    image_bytes = b""
        if result.get("isError"):
            try:
                detail = json.loads(text) if text else {}
            except json.JSONDecodeError:
                detail = {"message": text}
            return ErrResponse(
                code=result.get("arh_error_code", ""),
                detail=detail,
            )
        if image_bytes is not None:
            return OkResponse(payload=image_bytes)
        body = text.encode("utf-8") if text else b""
        return OkResponse(payload=body)

    def list_tools(self) -> List[dict]:
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


class WsWireClient(WireClient):
    """WireClient subclass that uses RFC 6455 framing post-hello."""

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
        body = super().hello(client_name=client_name, version=version,
                             framing="ws")
        self._mcp_initialize(client_name)
        return body

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
        fin = bool(b1 & self._FIN)
        if not fin:
            raise WireError("WS continuation frames are not supported")
        return opcode, payload


# ===========================================================================
# Bridge-only adapter
# ===========================================================================


def _create_connection(host: str, port: int, timeout: float) -> socket.socket:
    """Connect to (host, port), preferring IPv4 for ``*.local`` hostnames.

    Carried over from the v2.1 AgentClient. On dual-stack networks, Windows
    mDNS resolution of ``<host>.local`` typically returns multiple AAAA
    records ahead of the single A record; ``socket.create_connection``
    walks them in getaddrinfo order and can hang or time out on dead
    IPv6 addresses for the entire connect budget. For ``.local`` hosts we
    explicitly try AF_INET first; otherwise OS-preference ordering is fine.
    """
    norm = host.lower().rstrip(".")
    is_mdns = norm.endswith(".local")
    families = (
        [socket.AF_INET, socket.AF_INET6] if is_mdns else [socket.AF_UNSPEC]
    )

    last_err: Optional[OSError] = None
    for family in families:
        try:
            infos = socket.getaddrinfo(host, port, family, socket.SOCK_STREAM)
        except socket.gaierror as e:
            last_err = e
            continue
        for fam, type_, proto, _, sa in infos:
            sock: Optional[socket.socket] = None
            try:
                sock = socket.socket(fam, type_, proto)
                sock.settimeout(timeout)
                sock.connect(sa)
                return sock
            except OSError as e:
                last_err = e
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass

    if last_err is not None:
        raise last_err
    raise OSError(f"could not resolve {host!r}")


def read_token_file(path: Optional[str] = None) -> Optional[str]:
    """Reads the agent's elevation token. Returns None if the file isn't
    present or readable — callers should treat that as "auth disabled".

    Default path matches ``agents/windows-modern/src/config.cpp``'s
    ``default_token_path()``.
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


class AgentClient(WireClient):
    """v2.2 wire client adapter for the MCP bridge.

    Inherits the MCP-stdio framing from ``WireClient``; adds the lifecycle
    and tier-tracking surface the bridge expects:

    - ``connect()`` runs hello + the MCP initialize handshake (idempotent
      on an already-connected client).
    - ``current_tier`` / ``can_satisfy`` / ``set_token`` / ``tier_raise`` /
      ``tier_drop`` mirror the v2.1 client's external surface so tools.py
      doesn't need to learn a new contract.
    - Connect-time logic prefers IPv4 for ``*.local`` hostnames (mDNS
      AAAA-record death — see ARH issue #65).
    - ``wait_for_event(sub_id, timeout_s)`` translates the
      ``notifications/arh/event`` MCP notifications into the v2.1
      blocking-read-and-return-bytes contract used by the fire-once
      ``wait_for_*`` tools.

    Single-connection, single-thread by design. Tool calls serialise
    through one client; the lock protects the _next_id allocator + MCP
    frame readback against concurrent ``server.list_tools`` /
    ``server.call_tool`` invocations on the asyncio event loop.
    """

    PROTOCOL_VERSION = "2.2"

    def __init__(
        self,
        host: str,
        port: int,
        client_name: str = "mcp-server",
        timeout: float = 30.0,
    ) -> None:
        # NB: we deliberately bypass WireClient.__init__ so we can defer
        # socket creation until connect(). The v2.1 surface promised that
        # AgentClient(...) was a no-op until connect() — server.py relies
        # on that to format a clean error if the agent is unreachable.
        self.host = host
        self.port = port
        self.client_name = client_name
        self.timeout = timeout

        self._sock: Optional[socket.socket] = None
        self._buf = bytearray()
        self._next_id = 1
        self._initialized = False
        self.notifications: List[dict] = []
        self.hello_body: dict = {}
        self.server_info: dict = {}
        self._tools_cache: Optional[List[dict]] = None

        # Re-entrant: convenience wrappers (info / capabilities / tier_raise)
        # acquire the lock around a call to request(), which acquires the
        # lock again on the same thread — non-reentrant Lock would deadlock.
        self._lock = threading.RLock()
        self._current_tier: str = "read"
        self._token: Optional[str] = None
        self._cached_info: Optional[dict] = None

    # ------------------------------------------------------------------
    # Lifecycle

    def connect(self) -> None:
        """Open the socket, run the v2.2 hello + MCP initialize handshake."""
        if self._sock is not None:
            return
        self._sock = _create_connection(self.host, self.port, self.timeout)
        try:
            self.hello(client_name=self.client_name,
                       version=self.PROTOCOL_VERSION)
        except Exception:
            self.close()
            raise
        # Note the agent-reported tier rather than assuming read.
        try:
            info = self.info()
            self._cached_info = info
            self._current_tier = info.get("current_tier", "read")
        except Exception:
            pass

    def close(self) -> None:
        with self._lock:
            super().close()
            self._buf.clear()
            self._initialized = False

    # ------------------------------------------------------------------
    # Tier state

    @property
    def current_tier(self) -> str:
        return self._current_tier

    def can_satisfy(self, required: str) -> bool:
        """True if ``required`` is at or below the current tier on the v2.2
        CRUDX ladder (read < create < update < delete < extra_risky)."""
        order = {"read": 0, "create": 1, "update": 2,
                 "delete": 3, "extra_risky": 4}
        return order.get(self._current_tier, 0) >= order.get(required, 99)

    def set_token(self, token: str) -> None:
        self._token = token

    def tier_raise(self, target: str,
                   token: Optional[str] = None) -> Response:
        """Raise to ``target`` tier. If ``token`` is None, uses the cached
        token (set via ``set_token`` or read from the token file). On
        success, updates ``current_tier``."""
        tok = token or self._token
        if tok is None:
            return ErrResponse("auth_invalid",
                               {"message": "no token available"})
        r = super().tier_raise(target, tok)
        if isinstance(r, OkResponse):
            try:
                self._current_tier = r.json().get("new_tier", target)
            except Exception:
                self._current_tier = target
        return r

    def tier_drop(self, target: str = "read") -> Response:
        r = self.request("connection.tier_drop", "--tier", target)
        if isinstance(r, OkResponse):
            try:
                self._current_tier = r.json().get("new_tier", target)
            except Exception:
                self._current_tier = target
        return r

    # ------------------------------------------------------------------
    # Locked / cached convenience accessors

    def info(self) -> dict:
        with self._lock:
            return super().info()

    def capabilities(self) -> dict:
        with self._lock:
            return super().capabilities()

    def request(self, verb: str, *args: str, payload: bytes = b"") -> Response:
        """Thread-safe verb call (positional / flag style; back-compat with
        v2.1 callers)."""
        with self._lock:
            return super().request(verb, *args, payload=payload)

    def request_args(self, verb: str, arguments: Dict[str, Any]) -> Response:
        """Thread-safe verb call with a schema-correct arguments dict."""
        with self._lock:
            return super().request_args(verb, arguments)

    # ------------------------------------------------------------------
    # Subscription event support
    #
    # In v2.2 the wire-level `EVENT sub:N N\n…` frames are gone — events
    # arrive as MCP `notifications/arh/event` JSON-RPC notifications,
    # interleaved with normal request/response frames. The bridge's
    # fire-once `wait_for_*` tools need the v2.1 semantics back: block on
    # the connection until a notification with the matching subscription_id
    # arrives, return its event-data bytes, then let the caller cancel the
    # subscription. This method translates between the two.

    def wait_for_event(self, sub_id: str,
                       timeout_s: float) -> Optional[bytes]:
        """Block until a ``notifications/arh/event`` MCP notification
        arrives whose ``params.subscription_id`` equals ``sub_id``; return
        the JSON-encoded ``params.data`` bytes (or, for binary watch.region
        events, the raw image bytes). Returns None on timeout. Notifications
        for other subscriptions are buffered into ``self.notifications`` for
        diagnostic visibility.

        The caller holds the connection lock for the duration of the wait,
        so no other tool calls can complete on this AgentClient until
        ``wait_for_event`` returns. That matches MCP's serialised tool-call
        model.
        """
        deadline = time.time() + timeout_s
        with self._lock:
            if self._sock is None:
                raise WireError("not connected")
            original_timeout = self._sock.gettimeout()
            try:
                while True:
                    remaining = deadline - time.time()
                    if remaining <= 0:
                        return None
                    # BUFFER-CORRUPTION FIX: do NOT use a short timeout
                    # around `_read_mcp_frame` directly — if the timeout
                    # fires mid-frame (header read but body still
                    # incoming), the partially-read bytes are stranded
                    # in self._buf and the next read interprets them as
                    # garbage headers ("MCP frame missing Content-Length"
                    # at agent_client.py:265). The bug poisons the pipe
                    # permanently for every subsequent request_args call.
                    #
                    # Correct pattern: PEEK with a short timeout (truly
                    # interruptible — peek doesn't consume bytes), then
                    # once we know data is available, switch to BLOCKING
                    # and commit to reading the complete frame.
                    self._sock.settimeout(min(remaining, 1.0))
                    try:
                        peek = self._sock.recv(1, socket.MSG_PEEK)
                    except (socket.timeout, TimeoutError):
                        if time.time() >= deadline:
                            return None
                        continue
                    if not peek:
                        raise WireError("connection closed mid-wait")
                    # Data is queued. Read the complete frame with no
                    # timeout — short-timeout-mid-frame is the bug we're
                    # avoiding.
                    self._sock.settimeout(None)
                    try:
                        frame = self._read_mcp_frame()
                    finally:
                        # Restore the polling timeout for the next peek.
                        # `original_timeout` is restored on the outer
                        # `finally` when the method exits.
                        self._sock.settimeout(min(
                            max(deadline - time.time(), 0.001), 1.0))
                    method = frame.get("method")
                    if method == "notifications/arh/event":
                        params = frame.get("params") or {}
                        if params.get("subscription_id") == sub_id:
                            data = params.get("data")
                            if isinstance(data, str):
                                # Binary watch.region payloads arrive as
                                # base64 strings; let the caller decide
                                # how to interpret them. We return raw
                                # bytes — base64-decoded — so the existing
                                # tools.py handlers continue to base64-
                                # encode for return to the LLM.
                                try:
                                    return base64.b64decode(
                                        data, validate=True)
                                except (ValueError, TypeError):
                                    return data.encode("utf-8")
                            elif data is None:
                                return b""
                            return json.dumps(data).encode("utf-8")
                        # Notification for a different sub_id — buffer and
                        # keep waiting.
                        self.notifications.append(frame)
                        continue
                    # Non-event frame (other notification, server-initiated
                    # request, etc.) — buffer for diagnostics; ignore.
                    self.notifications.append(frame)
            finally:
                if self._sock is not None:
                    self._sock.settimeout(original_timeout)
