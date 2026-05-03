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

"""Tests for `connection.*` lifecycle verbs and the pre-hello state machine."""

from wire import ErrResponse, OkResponse, WireClient


def test_hello_succeeds(host: str, port: int) -> None:
    with WireClient(host, port) as c:
        c.hello()  # raises on failure


def test_pre_hello_rejects_other_verbs(host: str, port: int) -> None:
    with WireClient(host, port) as c:
        r = c.request("system.info")
        assert isinstance(r, ErrResponse)
        assert r.code == "invalid_state"


def test_protocol_mismatch_on_wrong_major(host: str, port: int) -> None:
    with WireClient(host, port) as c:
        r = c.request("connection.hello", "conformance", "99.0")
        assert isinstance(r, ErrResponse)
        assert r.code == "protocol_mismatch"


def test_close_returns_ok(host: str, port: int) -> None:
    with WireClient(host, port) as c:
        c.hello()
        r = c.request("connection.close")
        assert isinstance(r, OkResponse)


def test_reset_returns_ok(client: WireClient) -> None:
    r = client.request("connection.reset")
    assert isinstance(r, OkResponse)


def test_default_tier_is_read(client: WireClient) -> None:
    info = client.info()
    assert info["current_tier"] == "read"


def test_tier_raise_invalid_token_fails(client: WireClient) -> None:
    r = client.request("connection.tier_raise", "update", "not-the-token")
    assert isinstance(r, ErrResponse)
    assert r.code == "auth_invalid"


def test_tier_raise_unknown_tier_fails(client: WireClient) -> None:
    r = client.request("connection.tier_raise", "superuser", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_unmatched_quote_returns_invalid_args(host: str, port: int) -> None:
    """An unmatched opening `"` in the header should surface as
    ERR invalid_args per PROTOCOL.md §1.2.5. The bridge's high-level
    `WireClient.request()` won't produce this directly (it auto-quotes), so
    the test forms the malformed line at the socket level."""
    import socket as _socket
    s = _socket.create_connection((host, port), timeout=5.0)
    try:
        # Hello first to leave pre-hello state.
        s.sendall(b'connection.hello conformance 2.1\n')
        # Read OK 0.
        buf = b""
        while b"\n" not in buf:
            buf += s.recv(64)
        # Now send a malformed header — unmatched opening quote.
        s.sendall(b'system.info "argument-with-no-closing-quote\n')
        buf = b""
        while b"\n" not in buf:
            buf += s.recv(256)
        line = buf.split(b"\n", 1)[0].decode("utf-8")
        # Expected: 'ERR invalid_args <len>' followed by JSON detail.
        assert line.startswith("ERR invalid_args"), \
            f"expected ERR invalid_args, got: {line!r}"
    finally:
        s.close()
