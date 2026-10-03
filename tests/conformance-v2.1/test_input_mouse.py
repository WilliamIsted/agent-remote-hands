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

"""Tests for `input.mouse.*` (sub-namespace introduced in v2.1.0-rc.3).

Tier-gating + arg-validation only — exercising actual synthesised input
visibly perturbs the host and is left to manual / interactive testing."""

from conftest import needs_verb
from wire import ErrResponse, WireClient


# ---------------------------------------------------------------------------
# Tier gating — every input.mouse.* verb requires update tier

def test_input_mouse_click_requires_update_tier(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.click")
    r = client.request("input.mouse.click", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_mouse_move_requires_update_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.move")
    r = client.request("input.mouse.move", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_mouse_scroll_requires_update_tier(client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.scroll")
    r = client.request("input.mouse.scroll", "0", "0", "1")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_mouse_press_requires_update_tier(client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.press")
    r = client.request("input.mouse.press")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_mouse_release_requires_update_tier(client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.release")
    r = client.request("input.mouse.release")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_mouse_drag_requires_update_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.drag")
    r = client.request("input.mouse.drag", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


# ---------------------------------------------------------------------------
# Arg validation — these run past the tier gate and exercise the verb's
# arg parser. Use update_client; off-screen coords keep the host unperturbed
# if the rejection path ever regresses.

def test_input_mouse_click_unknown_flag_rejected(update_client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.click")
    r = update_client.request("input.mouse.click",
                              "-9999", "-9999", "--bogus-flag")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args"
    assert r.detail.get("unknown_flag") == "--bogus-flag"


def test_input_mouse_click_double_and_duration_mutually_exclusive(
        update_client: WireClient, capabilities: dict) -> None:
    """Per the input.mouse.click `x-mutually-exclusive: [double, duration_ms]`
    constraint — a duration on a double-click is ambiguous so the verb
    must reject combining them."""
    needs_verb(capabilities, "input.mouse.click")
    r = update_client.request("input.mouse.click", "-9999", "-9999",
                              "--double", "--duration-ms", "100")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args"


def test_input_mouse_click_duration_ms_out_of_range(
        update_client: WireClient, capabilities: dict) -> None:
    """duration_ms is clamped to [0, 1000]. >1000 must be rejected."""
    needs_verb(capabilities, "input.mouse.click")
    r = update_client.request("input.mouse.click", "-9999", "-9999",
                              "--duration-ms", "5000")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_input_mouse_click_invalid_button(update_client: WireClient,
                                          capabilities: dict) -> None:
    """button enum is left/right/middle — 'wheel' is invalid."""
    needs_verb(capabilities, "input.mouse.click")
    r = update_client.request("input.mouse.click", "-9999", "-9999",
                              "--button", "wheel")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_input_mouse_drag_unknown_flag_rejected(update_client: WireClient,
                                                capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.drag")
    r = update_client.request("input.mouse.drag", "-9999", "-9999",
                              "--bogus-flag")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"
    assert r.detail.get("unknown_flag") == "--bogus-flag"
