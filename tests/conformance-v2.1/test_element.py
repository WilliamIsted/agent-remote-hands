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

"""Tests for `element.*` (UI Automation).

Wire-shape note: post-rc.2 the element handle field is `handle` (not `id`)
and matcher inputs use `name` (case-insensitive substring) plus the optional
`automation_id` and `role` selectors. The `elt:N` handle prefix is unchanged."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# element.at — read-tier hit-test

def test_element_at_hit_test_top_left(client: WireClient,
                                      capabilities: dict) -> None:
    """Hit-test the desktop origin; should land on something or be uia_blind."""
    needs_verb(capabilities, "element.at")
    r = client.request("element.at", "0", "0")
    assert isinstance(r, (OkResponse, ErrResponse))
    if isinstance(r, OkResponse):
        body = json.loads(r.payload)
        assert "handle" in body
        assert body["handle"].startswith("elt:")
        assert "bounds" in body
        for k in ("x", "y", "w", "h"):
            assert k in body["bounds"]


# ---------------------------------------------------------------------------
# element.list — read-tier subtree walk

def test_element_list_returns_elements_array(client: WireClient,
                                             capabilities: dict) -> None:
    """Restrict to a small region for a fast call."""
    needs_verb(capabilities, "element.list")
    r = client.request("element.list",
                       "--region", "0,0,200,200")
    # Either OK with elements or uia_blind / permission_denied.
    if isinstance(r, OkResponse):
        body = json.loads(r.payload)
        assert "elements" in body
        assert isinstance(body["elements"], list)
        for e in body["elements"][:3]:
            assert e["handle"].startswith("elt:")
            for k in ("x", "y", "w", "h"):
                assert k in e["bounds"]


# ---------------------------------------------------------------------------
# element.find — name / automation_id matchers

def test_element_find_unknown_returns_not_found(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "element.find")
    r = client.request("element.find",
                       "--name", "DefinitelyNotARealElementName123",
                       "--timeout-ms", "0")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "uia_blind")


def test_element_find_name_and_automation_id_mutually_exclusive(
        client: WireClient, capabilities: dict) -> None:
    """Per the spec's `x-mutually-exclusive: [name, automation_id]`."""
    needs_verb(capabilities, "element.find")
    r = client.request("element.find",
                       "--name", "Foo", "--automation-id", "Bar",
                       "--timeout-ms", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# element.wait — polling form of find with flags_required

def test_element_wait_unknown_times_out(client: WireClient,
                                        capabilities: dict) -> None:
    """Use a short timeout so the suite stays fast."""
    needs_verb(capabilities, "element.wait")
    r = client.request("element.wait",
                       "--name", "DefinitelyNotARealElementName123",
                       "--timeout-ms", "200")
    assert isinstance(r, ErrResponse)
    # `not_found` is the documented code for both no-match-on-first-poll
    # AND timeout (per element.wait.x-errors).
    assert r.code in ("not_found", "timeout", "uia_blind")


def test_element_wait_invalid_flags_required(client: WireClient,
                                              capabilities: dict) -> None:
    """flags_required must be drawn from the universal-state enum."""
    needs_verb(capabilities, "element.wait")
    r = client.request("element.wait",
                       "--name", "anything",
                       "--flags-required", "this-is-not-a-valid-flag",
                       "--timeout-ms", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# element.tree — depth-tagged pre-order traversal

def test_element_tree_invalid_handle(client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "element.tree")
    r = client.request("element.tree", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code in ("target_gone", "invalid_args")


# ---------------------------------------------------------------------------
# Update-tier action verbs — tier-gating only (no real elements to hit)

def test_element_invoke_requires_update_tier(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "element.invoke")
    r = client.request("element.invoke", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_toggle_requires_update_tier(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "element.toggle")
    r = client.request("element.toggle", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_expand_requires_update_tier(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "element.expand")
    r = client.request("element.expand", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_collapse_requires_update_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "element.collapse")
    r = client.request("element.collapse", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_focus_requires_update_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "element.focus")
    r = client.request("element.focus", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_set_text_requires_update_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "element.set_text")
    r = client.request("element.set_text", "elt:99999", "test")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_find_invoke_requires_update_tier(client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "element.find_invoke")
    r = client.request("element.find_invoke",
                       "--name", "DefinitelyNotARealElementName123")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_element_at_invoke_requires_update_tier(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "element.at_invoke")
    r = client.request("element.at_invoke", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


# ---------------------------------------------------------------------------
# element.text — read-tier text reader

def test_element_text_invalid_handle(client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "element.text")
    r = client.request("element.text", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code in ("target_gone", "not_supported_by_target", "invalid_args")
