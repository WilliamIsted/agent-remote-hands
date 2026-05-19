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

"""Tests for `input.keyboard.*` (sub-namespace introduced in v2.1.0-rc.3).

Tier-gating + arg-validation only. Synthesised keys would visibly perturb
the host (whatever the focused window happens to be) — actual input is
left to manual / interactive testing. F24 is used as the gate-probe vk
since few apps bind it."""

from conftest import needs_verb
from wire import ErrResponse, WireClient


# ---------------------------------------------------------------------------
# Tier gating — every input.keyboard.* verb requires update tier

def test_input_keyboard_key_requires_update_tier(client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key")
    r = client.request("input.keyboard.key", "F24")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_keyboard_type_requires_update_tier(client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.type")
    r = client.request("input.keyboard.type", "")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_keyboard_key_down_requires_update_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key_down")
    r = client.request("input.keyboard.key_down", "F24")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_keyboard_key_up_requires_update_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key_up")
    r = client.request("input.keyboard.key_up", "F24")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


# ---------------------------------------------------------------------------
# Arg validation

def test_input_keyboard_key_unknown_vk_rejected(update_client: WireClient,
                                                capabilities: dict) -> None:
    """Unknown vk names must be rejected, not silently no-op'd."""
    needs_verb(capabilities, "input.keyboard.key")
    r = update_client.request("input.keyboard.key", "definitely-not-a-key")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_input_keyboard_key_unknown_flag_rejected(update_client: WireClient,
                                                  capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key")
    r = update_client.request("input.keyboard.key", "F24", "--bogus-flag")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"
    assert r.detail.get("unknown_flag") == "--bogus-flag"


def test_input_keyboard_key_duration_ms_out_of_range(
        update_client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key")
    r = update_client.request("input.keyboard.key", "F24",
                              "--duration-ms", "5000")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_input_keyboard_key_up_idempotent_when_not_held(
        update_client: WireClient, capabilities: dict) -> None:
    """Per the spec, key_up is idempotent — releasing a key that was never
    held returns OK 0, not ERR not_held. F24 is never held by anything in
    this test suite, so this exercises the cleanup-fail-safe path."""
    needs_verb(capabilities, "input.keyboard.key_up")
    from wire import OkResponse
    r = update_client.request("input.keyboard.key_up", "F24")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
