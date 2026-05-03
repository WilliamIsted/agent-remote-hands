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

"""Tests for `window.*`."""

import json

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_window_list_returns_array(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    r = client.request("window.list")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "windows" in body
    assert isinstance(body["windows"], list)


def test_window_list_entries_have_required_fields(client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    r = client.request("window.list", "--all")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    for w in body["windows"][:5]:
        for key in ["hwnd", "x", "y", "w", "h", "title", "pid"]:
            assert key in w, f"window entry missing {key}: {w}"


def test_window_focus_requires_update_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "window.focus")
    r = client.request("window.focus", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_window_state_on_invalid_hwnd(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "window.state")
    r = client.request("window.state", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "target_gone"
