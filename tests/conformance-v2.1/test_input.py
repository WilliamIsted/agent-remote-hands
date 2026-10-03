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

"""Tests for top-level `input.*` verbs (post-rc.3 split). The mouse and
keyboard sub-namespaces have their own files: test_input_mouse.py,
test_input_keyboard.py."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# input.position — read-tier cursor query

def test_input_position_returns_x_y(client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "input.position")
    r = client.request("input.position")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert isinstance(body.get("x"), int)
    assert isinstance(body.get("y"), int)
    # monitor_index is opt-in; absent unless include_monitor was passed.
    assert "monitor_index" not in body


def test_input_position_with_monitor_index(client: WireClient,
                                           capabilities: dict) -> None:
    """`--include-monitor` adds a 0-based monitor_index field."""
    needs_verb(capabilities, "input.position")
    r = client.request("input.position", "--include-monitor")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "monitor_index" in body
    assert isinstance(body["monitor_index"], int)
    assert body["monitor_index"] >= 0


# ---------------------------------------------------------------------------
# input.send_message — synchronous Win32 message escape hatch

def test_input_send_message_requires_update_tier(client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "input.send_message")
    r = client.request("input.send_message", "win:0x1", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_send_message_invalid_handle(update_client: WireClient,
                                           capabilities: dict) -> None:
    """A bogus handle should error rather than blocking on the wndproc."""
    needs_verb(capabilities, "input.send_message")
    r = update_client.request("input.send_message", "win:0xFFFFFFFF", "0")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")


# ---------------------------------------------------------------------------
# input.post_message — non-blocking peer of send_message

def test_input_post_message_requires_update_tier(client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "input.post_message")
    r = client.request("input.post_message", "win:0x1", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_post_message_invalid_handle(update_client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "input.post_message")
    r = update_client.request("input.post_message", "win:0xFFFFFFFF", "0")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")
