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

"""Conformance tests for the v2.2 framing modes (`mcp` and `ws`).

Two fixture families:

- The standard `client` fixture is itself MCP-stdio (the v2.2 default), so
  every MCP-mode assertion lives here.
- The `ws_client` fixture skips when the agent does not advertise `"ws"`
  in `system.info.framings`; ws-specific assertions live there.
"""

import json
import socket

import pytest

from test_system import REQUIRED_INFO_FIELDS
from wire import ErrResponse, OkResponse, WireClient, WireError, WsWireClient


# ---------------------------------------------------------------------------
# Bootstrap-line tests — drive a raw socket, never call WireClient.hello() so
# we can observe the bootstrap framing directly.

def _send_raw_hello(host: str, port: int, line: str) -> tuple:
    """Send a raw `connection.hello` line and return (head, tail, body) where
    head is the leading directive ('OK' / 'ERR'), tail is the rest of the
    response line, and body is the OK/ERR payload bytes."""
    sock = socket.create_connection((host, port), timeout=5.0)
    try:
        sock.sendall(line.encode("utf-8") + b"\n")
        buf = bytearray()
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                raise WireError("connection closed mid-line")
            buf.extend(chunk)
        idx = buf.index(b"\n")
        line_bytes = bytes(buf[:idx])
        del buf[: idx + 1]
        if line_bytes.endswith(b"\r"):
            line_bytes = line_bytes[:-1]
        parts = line_bytes.decode("utf-8").split(" ", 2)
        head = parts[0]
        if head == "OK":
            length = int(parts[1]) if len(parts) > 1 else 0
            tail = ""
        elif head == "ERR":
            tail = parts[1] if len(parts) > 1 else ""
            length = int(parts[2]) if len(parts) > 2 else 0
        else:
            raise WireError(f"unexpected directive: {head}")
        while len(buf) < length:
            chunk = sock.recv(min(65536, length - len(buf)))
            if not chunk:
                raise WireError("connection closed mid-payload")
            buf.extend(chunk)
        body = bytes(buf[:length])
        return head, tail, body
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# `client` fixture (always runs — WireClient is MCP under the hood)

def test_hello_body_has_framing_field(host: str, port: int) -> None:
    head, _, body = _send_raw_hello(
        host, port, "connection.hello conformance 2.2")
    assert head == "OK", f"got ERR: {body!r}"
    parsed = json.loads(body)
    assert "framing" in parsed, f"hello body missing framing: {parsed}"


def test_hello_default_framing_is_mcp_for_v22(host: str, port: int) -> None:
    head, _, body = _send_raw_hello(
        host, port, "connection.hello conformance 2.2")
    assert head == "OK"
    parsed = json.loads(body)
    assert parsed["framing"] == "mcp", \
        f"v2.2 default framing should be mcp; got {parsed['framing']!r}"


def test_hello_v21_gets_protocol_mismatch(host: str, port: int) -> None:
    """v2.2+ agents do not advertise v2.1 as supported."""
    head, code, _ = _send_raw_hello(
        host, port, "connection.hello conformance 2.1")
    assert head == "ERR", f"v2.1 should be rejected, got {head}"
    assert code == "protocol_mismatch", f"got {code!r}"


def test_hello_unknown_framing_returns_error(host: str, port: int) -> None:
    head, code, _ = _send_raw_hello(
        host, port, "connection.hello conformance 2.2 --framing bogus")
    assert head == "ERR"
    assert code == "framing_unsupported", f"got {code!r}"


def test_system_info_has_framings_field(client: WireClient) -> None:
    info = client.info()
    assert "framings" in info, f"system.info missing framings: {info}"
    assert isinstance(info["framings"], list)
    assert info["framings"], "framings list must be non-empty on a v2.2 agent"
    assert "mcp" in info["framings"], \
        f"v2.2 agents must advertise mcp; got {info['framings']!r}"


def test_mcp_initialize_server_info(client: WireClient) -> None:
    """The hello + initialize handshake ran during the fixture; serverInfo
    must have been reported."""
    assert client.server_info, "MCP serverInfo missing after initialize"
    assert "name" in client.server_info, \
        f"serverInfo.name missing: {client.server_info}"


def test_mcp_tools_list_has_system_info(client: WireClient) -> None:
    tools = client.list_tools()
    names = {t["name"] for t in tools}
    assert "system.info" in names, \
        f"tools/list missing system.info: {sorted(names)}"


def test_mcp_tools_list_superset_of_capabilities(
        client: WireClient, capabilities: dict) -> None:
    tools = client.list_tools()
    served = {t["name"] for t in tools}
    advertised = set(capabilities.keys())
    # Excluded verbs (§1.6.7) need not appear in tools/list.
    excluded = {"connection.hello", "connection.close",
                "connection.reset", "system.verbs"}
    missing = (advertised - served) - excluded
    assert not missing, \
        f"capabilities advertises verbs absent from tools/list: {sorted(missing)}"


def test_mcp_excluded_verbs_not_in_tools_list(client: WireClient) -> None:
    tools = client.list_tools()
    names = {t["name"] for t in tools}
    for excluded in ("connection.hello", "connection.close", "system.verbs"):
        assert excluded not in names, \
            f"{excluded} should be excluded from tools/list per §1.6.7"


def test_mcp_tier_enforcement(
        client: WireClient, capabilities: dict) -> None:
    """Calling an update-tier verb at read tier must return isError:true with
    arh_error_code == 'tier_required'."""
    if "input.mouse.click" not in capabilities:
        pytest.skip("agent does not advertise input.mouse.click")
    r = client.request("input.mouse.click", "--x", "0", "--y", "0")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "tier_required", \
        f"expected tier_required, got {r.code!r}"


# ---------------------------------------------------------------------------
# `ws_client` fixture — skips when the agent does not advertise ws

def test_ws_hello_response_has_framing_ws(ws_client: WsWireClient) -> None:
    assert ws_client.hello_body.get("framing") == "ws", \
        f"ws hello body framing != ws: {ws_client.hello_body}"


def test_ws_system_info_required_fields(ws_client: WsWireClient) -> None:
    info = ws_client.info()
    missing = REQUIRED_INFO_FIELDS - set(info)
    assert not missing, f"system.info over ws missing: {missing}"


def test_ws_framings_includes_ws(ws_client: WsWireClient) -> None:
    info = ws_client.info()
    assert "ws" in info.get("framings", []), \
        f"ws connection's system.info.framings should include ws: {info.get('framings')!r}"


def test_ws_tier_enforcement(
        ws_client: WsWireClient, capabilities: dict) -> None:
    if "input.mouse.click" not in capabilities:
        pytest.skip("agent does not advertise input.mouse.click")
    r = ws_client.request("input.mouse.click", "--x", "0", "--y", "0")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "tier_required", \
        f"expected tier_required, got {r.code!r}"
