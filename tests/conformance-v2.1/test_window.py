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

"""Tests for `window.*`.

Wire-shape note: post-rc.2 the `window.list` response is a BARE ARRAY
of `{handle, title, pid, bounds: {x,y,w,h}}` items — no `{windows: [...]}` wrap.
The handle field is `handle` (cross-OS-friendly), not `hwnd`."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# window.list — bare-array shape with nested bounds

def test_window_list_returns_bare_array(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    r = client.request("window.list")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert isinstance(body, list), \
        "window.list response is a bare array, not {windows: [...]}"


def test_window_list_entries_have_required_fields(client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    r = client.request("window.list", "--visible-only", "false")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)
    assert isinstance(entries, list)
    for w in entries[:5]:
        assert "handle" in w, f"entry missing handle: {w}"
        assert w["handle"].startswith("win:"), \
            f"handle should be 'win:0x...': {w['handle']!r}"
        assert "title" in w
        assert isinstance(w["pid"], int)
        for k in ("x", "y", "w", "h"):
            assert k in w["bounds"], f"bounds missing {k}: {w['bounds']}"


def test_window_list_pid_filter_narrows_results(client: WireClient,
                                                capabilities: dict) -> None:
    """If a `--pid` filter is supplied, every returned entry's pid matches."""
    needs_verb(capabilities, "window.list")
    # Get any pid first.
    r = client.request("window.list")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)
    if not entries:
        return  # No windows visible; can't pick a pid.
    pid = entries[0]["pid"]

    r = client.request("window.list", "--pid", str(pid))
    assert isinstance(r, OkResponse)
    filtered = json.loads(r.payload)
    for e in filtered:
        assert e["pid"] == pid


def test_window_list_include_monitor_adds_index(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    r = client.request("window.list", "--include-monitor")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)
    for e in entries[:3]:
        assert "monitor_index" in e
        assert isinstance(e["monitor_index"], int)
        assert e["monitor_index"] >= 0


# ---------------------------------------------------------------------------
# window.find — match modes (substring/prefix/exact/glob/regex)

def test_window_find_unknown_returns_not_found(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "window.find")
    r = client.request("window.find",
                       "DefinitelyNotARealWindowTitle-" * 4)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_window_find_unknown_match_mode_rejected(client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "window.find")
    r = client.request("window.find", "anything", "--match", "fuzzy")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_window_find_returns_required_fields(client: WireClient,
                                             capabilities: dict) -> None:
    """If any window matches, the returned entry has the full shape."""
    needs_verb(capabilities, "window.list")
    needs_verb(capabilities, "window.find")
    listing = json.loads(client.request("window.list").payload)
    if not listing:
        return  # No windows to match against.
    # Pick a non-empty title so the substring search has something to find.
    pick = next((e for e in listing if e["title"]), None)
    if pick is None:
        return  # All visible windows have empty titles — unusual but tolerable.

    r = client.request("window.find", pick["title"])
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    for k in ("handle", "title", "pid", "bounds"):
        assert k in body, f"window.find result missing {k}: {body}"
    assert body["handle"].startswith("win:")


# ---------------------------------------------------------------------------
# window.focus — update tier; new_tier-symmetric prior_handle response

def test_window_focus_requires_update_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "window.focus")
    r = client.request("window.focus", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_window_focus_invalid_handle(update_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "window.focus")
    r = update_client.request("window.focus", "win:0xFFFFFFFF")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")


# ---------------------------------------------------------------------------
# window.close — update tier

def test_window_close_requires_update_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "window.close")
    r = client.request("window.close", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_window_close_invalid_handle(update_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "window.close")
    r = update_client.request("window.close", "win:0xFFFFFFFF")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")


# ---------------------------------------------------------------------------
# window.move — update tier; mock-up response shape (bounds + prior_bounds + foreground_status)

def test_window_move_requires_update_tier(client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "window.move")
    r = client.request("window.move", "win:0x1", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_window_move_invalid_handle(update_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "window.move")
    r = update_client.request("window.move", "win:0xFFFFFFFF", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")


# ---------------------------------------------------------------------------
# window.state — read-only enum

def test_window_state_on_invalid_handle(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "window.state")
    r = client.request("window.state", "win:0xFFFFFFFF")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "target_gone", "invalid_args")


def test_window_state_on_real_window_has_known_enum(
        client: WireClient, capabilities: dict) -> None:
    """Pick any visible window and assert the state enum is one of the four
    canonical values."""
    needs_verb(capabilities, "window.list")
    needs_verb(capabilities, "window.state")
    entries = json.loads(client.request("window.list").payload)
    if not entries:
        return
    handle = entries[0]["handle"]
    r = client.request("window.state", handle)
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    assert body["state"] in {"minimised", "maximised", "normal", "hidden"}
