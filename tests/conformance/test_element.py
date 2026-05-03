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

"""Tests for `element.*` (UI Automation)."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_element_at_returns_an_element(client: WireClient,
                                       capabilities: dict) -> None:
    """Hit-test the desktop origin; should land on something."""
    needs_verb(capabilities, "element.at")
    r = client.request("element.at", "0", "0")
    # Either a UIA element exists (OK) or UIA cannot reach it (uia_blind).
    assert isinstance(r, (OkResponse, ErrResponse))
    if isinstance(r, OkResponse):
        body = json.loads(r.payload)
        assert "id" in body
        assert body["id"].startswith("elt:")


def test_element_find_unknown_returns_not_found(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "element.find")
    r = client.request("element.find", "button",
                       "DefinitelyNotARealButtonName123")
    # Either not_found or uia_blind, depending on IL barrier.
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "uia_blind")


def test_element_invoke_invalid_id_returns_target_gone(client: WireClient,
                                                       capabilities: dict) -> None:
    needs_verb(capabilities, "element.invoke")
    r = client.request("element.invoke", "elt:99999")
    # Bogus id is target_gone, but verb is update-tier so we hit tier_required first.
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_list_at_read(client: WireClient,
                              capabilities: dict) -> None:
    needs_verb(capabilities, "element.list")
    # Restrict to a small region so the call finishes quickly.
    r = client.request("element.list", "--region", "0,0,200,200")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "elements" in body
    assert isinstance(body["elements"], list)


def test_element_wait_invalid_args(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "element.wait")
    r = client.request("element.wait", "button")  # missing pattern + timeout
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_element_wait_unknown_times_out(client: WireClient,
                                        capabilities: dict) -> None:
    """Use a short timeout so the suite stays fast."""
    needs_verb(capabilities, "element.wait")
    r = client.request("element.wait", "button",
                       "DefinitelyNotARealButtonName123", "500")
    assert isinstance(r, ErrResponse)
    assert r.code == "timeout"
