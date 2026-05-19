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

"""Tests for `registry.value.*` and `registry.key.*`.

Wire-shape note: post-rc.2 the registry namespace was restructured into
resource-first CRUD: `registry.value.read/create/update/delete` for individual
values, and `registry.key.read/delete` for whole keys. The pre-rc.2 verbs
(`registry.read` / `registry.write` / `registry.delete`) no longer exist."""

import json
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# A read-only, always-present key on every Windows system.
_KNOWN_KEY = r"HKLM\Software\Microsoft\Windows NT\CurrentVersion"


def _scratch_key() -> str:
    r"""A scratch key under HKCU\Software (writable without admin)."""
    return rf"HKCU\Software\AgentRemoteHandsConformance-{uuid.uuid4().hex}"


# ---------------------------------------------------------------------------
# registry.value.read

def test_value_read_known_value(client: WireClient,
                                capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.read")
    r = client.request("registry.value.read", _KNOWN_KEY, "ProductName")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["type"] == "REG_SZ"
    assert isinstance(body["data"], str)
    assert body["data"]  # non-empty


def test_value_read_unknown_returns_not_found(client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.read")
    r = client.request("registry.value.read", _KNOWN_KEY,
                       "DefinitelyNotARealValue-" + uuid.uuid4().hex)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_value_read_invalid_root_returns_invalid_args(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.read")
    r = client.request("registry.value.read", r"INVALID\Foo\Bar", "")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# registry.key.read — split out of pre-rc.2 registry.read whole-key mode

def test_key_read_known_key(client: WireClient,
                            capabilities: dict) -> None:
    needs_verb(capabilities, "registry.key.read")
    r = client.request("registry.key.read", _KNOWN_KEY)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "subkeys" in body and isinstance(body["subkeys"], list)
    assert "values" in body and isinstance(body["values"], list)
    # Per the spec the values list returns names + types only — not data.
    for v in body["values"]:
        assert "name" in v and "type" in v
        assert "data" not in v, \
            "registry.key.read must not include value data; use registry.value.read"


def test_key_read_unknown_returns_not_found(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "registry.key.read")
    r = client.request("registry.key.read",
                       rf"HKCU\Software\NoSuchKey-{uuid.uuid4().hex}")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


# ---------------------------------------------------------------------------
# Tier gating — value.create / value.update / value.delete / key.delete

def test_value_create_requires_create_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.create")
    r = client.request("registry.value.create",
                       _scratch_key(), "TestValue", "REG_DWORD", "42")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_value_update_requires_update_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.update")
    r = client.request("registry.value.update",
                       _scratch_key(), "TestValue", "REG_DWORD", "42")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_value_delete_requires_delete_tier(update_client: WireClient,
                                           capabilities: dict) -> None:
    """Update tier is insufficient — value.delete needs delete tier."""
    needs_verb(capabilities, "registry.value.delete")
    r = update_client.request("registry.value.delete",
                              _scratch_key(), "Nonexistent")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "delete"


def test_key_delete_requires_delete_tier(update_client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "registry.key.delete")
    r = update_client.request("registry.key.delete", _scratch_key())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "delete"


# ---------------------------------------------------------------------------
# Round-trips at the appropriate tiers

def test_value_create_then_update_then_delete(delete_client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.update")
    needs_verb(capabilities, "registry.value.delete")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    try:
        # Create — first call succeeds.
        r = delete_client.request("registry.value.create",
                                  key, "TestVal", "REG_DWORD", "42")
        assert isinstance(r, OkResponse), f"create failed: {r!r}"

        # Create again on the same value must fail with already_exists.
        r = delete_client.request("registry.value.create",
                                  key, "TestVal", "REG_DWORD", "99")
        assert isinstance(r, ErrResponse)
        assert r.code == "already_exists"

        # Read back the original.
        r = delete_client.request("registry.value.read", key, "TestVal")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_DWORD"
        assert body["data"] == "42"

        # Update — succeeds because the value exists.
        r = delete_client.request("registry.value.update",
                                  key, "TestVal", "REG_DWORD", "99")
        assert isinstance(r, OkResponse)

        # Read back the new value.
        r = delete_client.request("registry.value.read", key, "TestVal")
        assert isinstance(r, OkResponse)
        assert json.loads(r.payload)["data"] == "99"

        # Delete — succeeds.
        r = delete_client.request("registry.value.delete", key, "TestVal")
        assert isinstance(r, OkResponse)

        # Subsequent read returns not_found.
        r = delete_client.request("registry.value.read", key, "TestVal")
        assert isinstance(r, ErrResponse)
        assert r.code == "not_found"
    finally:
        # Tear down the scratch key (created implicitly by value.create).
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_update_missing_returns_not_found(delete_client: WireClient,
                                                capabilities: dict) -> None:
    """update against a missing value/key fails with not_found
    (vs. create's already_exists for the inverse case)."""
    needs_verb(capabilities, "registry.value.update")
    r = delete_client.request("registry.value.update",
                              _scratch_key(), "NoSuchValue",
                              "REG_DWORD", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"
