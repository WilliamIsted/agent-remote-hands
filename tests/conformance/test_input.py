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

"""Tests for `input.*`. Tier-gating only — exercising actual input is left to
manual + ad-hoc fixtures, since synthetic input visibly perturbs the host."""

from conftest import needs_verb
from wire import ErrResponse, WireClient


def test_input_click_requires_drive_tier(client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "input.click")
    r = client.request("input.click", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_move_requires_drive_tier(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "input.move")
    r = client.request("input.move", "0", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_key_requires_drive_tier(client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "input.key")
    r = client.request("input.key", "F24")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_input_type_requires_drive_tier(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "input.type")
    r = client.request("input.type", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
