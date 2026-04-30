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

"""Tests for `registry.*`."""

import json
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_read_known_key(client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "registry.read")
    r = client.request("registry.read",
                       r"HKLM\Software\Microsoft\Windows NT\CurrentVersion")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "values" in body
    assert "subkeys" in body


def test_read_specific_value(client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "registry.read")
    r = client.request(
        "registry.read",
        r"HKLM\Software\Microsoft\Windows NT\CurrentVersion",
        "--value", "ProductName",
    )
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["type"] == "REG_SZ"
    assert "data" in body


def test_read_unknown_key_returns_not_found(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "registry.read")
    r = client.request(
        "registry.read",
        rf"HKCU\Software\AgentRemoteHandsConformance-{uuid.uuid4().hex}",
    )
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_invalid_root_returns_invalid_args(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "registry.read")
    r = client.request("registry.read", r"INVALID\Foo\Bar")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_registry_read_unknown_flag_rejected(client: WireClient,
                                             capabilities: dict) -> None:
    """The path-join branch in registry.read must still reject unknown
    --flags. This exercises the same contract gap as screen.capture but on
    a verb whose arg loop has additional positional-collection logic."""
    needs_verb(capabilities, "registry.read")
    r = client.request("registry.read",
                       r"HKLM\Software\Microsoft", "--bogus-flag")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args"
    assert r.detail.get("unknown_flag") == "--bogus-flag"


def test_write_requires_drive_tier(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "registry.write")
    r = client.request(
        "registry.write",
        r"HKCU\Software\AgentRemoteHandsConformance",
        "TestValue", "REG_DWORD", "42",
    )
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_delete_requires_power_tier(drive_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "registry.delete")
    r = drive_client.request(
        "registry.delete",
        r"HKCU\Software\AgentRemoteHandsConformance",
        "--value", "Nonexistent",
    )
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "power"
