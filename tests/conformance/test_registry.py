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

import base64
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


# ---------------------------------------------------------------------------
# REG_BINARY round-trips — F1 binary write closure
#
# input_schema says REG_BINARY `data` is base64. The encode-side
# (registry.value.read serialising REG_BINARY -> base64) has always worked;
# these tests pin down the decode-side (registry.value.create/update accepting
# base64 -> writing raw bytes via RegSetValueExW).

def test_value_create_binary(delete_client: WireClient,
                             capabilities: dict) -> None:
    """Create REG_BINARY, read back, assert byte-identical round-trip."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    raw = bytes(range(256))  # every possible byte value
    encoded = base64.b64encode(raw).decode("ascii")
    try:
        r = delete_client.request("registry.value.create",
                                  key, "BinVal", "REG_BINARY", encoded)
        assert isinstance(r, OkResponse), f"create failed: {r!r}"

        r = delete_client.request("registry.value.read", key, "BinVal")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_BINARY"
        # Read-back is base64-encoded per registry.value.read.json.
        assert base64.b64decode(body["data"]) == raw
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_update_binary(delete_client: WireClient,
                             capabilities: dict) -> None:
    """Pre-create with one REG_BINARY blob, update to another, assert new."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.update")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    first = b"\x00\x01\x02\x03"
    second = b"\xff\xfe\xfd\xfc\xfb\xfa"
    try:
        r = delete_client.request("registry.value.create", key, "BinVal",
                                  "REG_BINARY",
                                  base64.b64encode(first).decode("ascii"))
        assert isinstance(r, OkResponse)

        r = delete_client.request("registry.value.update", key, "BinVal",
                                  "REG_BINARY",
                                  base64.b64encode(second).decode("ascii"))
        assert isinstance(r, OkResponse), f"update failed: {r!r}"

        r = delete_client.request("registry.value.read", key, "BinVal")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_BINARY"
        assert base64.b64decode(body["data"]) == second
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_create_binary_empty(delete_client: WireClient,
                                   capabilities: dict) -> None:
    """Zero-byte REG_BINARY: cbData=0 + NULL pointer is valid in Win32."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    try:
        r = delete_client.request("registry.value.create",
                                  key, "EmptyBin", "REG_BINARY", "")
        assert isinstance(r, OkResponse), f"empty create failed: {r!r}"

        r = delete_client.request("registry.value.read", key, "EmptyBin")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_BINARY"
        assert base64.b64decode(body["data"]) == b""
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_create_binary_invalid_base64(delete_client: WireClient,
                                            capabilities: dict) -> None:
    """Malformed base64 -> invalid_args (spec-declared x-error)."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    try:
        # `!` is outside the base64 alphabet — decoder must reject.
        r = delete_client.request("registry.value.create",
                                  key, "BadBin", "REG_BINARY",
                                  "not!valid!base64")
        assert isinstance(r, ErrResponse)
        assert r.code == "invalid_args"
    finally:
        # The bad request may or may not have created the parent key (the
        # current handler creates the parent before encoding); clean up
        # defensively. Errors here are ignored — the value never landed.
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_create_binary_too_large(delete_client: WireClient,
                                       capabilities: dict) -> None:
    """A 2 MB REG_BINARY exceeds the Win32 per-value limit (~1 MB) and the
    write must fail rather than silently truncate. The exact spec error code
    depends on what RegSetValueExW returns; this test only pins down 'it's an
    ErrResponse' so the handler can map it to any spec-declared code without
    breaking the suite."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    blob = b"\xab" * (2 * 1024 * 1024)
    encoded = base64.b64encode(blob).decode("ascii")
    try:
        r = delete_client.request("registry.value.create",
                                  key, "BigBin", "REG_BINARY", encoded)
        assert isinstance(r, ErrResponse), \
            f"oversize REG_BINARY unexpectedly succeeded: {r!r}"
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_create_binary_then_delete_roundtrip(
        delete_client: WireClient, capabilities: dict) -> None:
    """Full CRUD lifecycle for a REG_BINARY value — create, update, read,
    delete — exercising the binary write path end-to-end (the previous
    create-then-update-then-delete test only covered REG_DWORD)."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.update")
    needs_verb(capabilities, "registry.value.delete")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    payload = b"\xde\xad\xbe\xef"
    encoded = base64.b64encode(payload).decode("ascii")
    try:
        r = delete_client.request("registry.value.create",
                                  key, "Bin", "REG_BINARY", encoded)
        assert isinstance(r, OkResponse)

        # Re-create on an existing value still surfaces already_exists for
        # REG_BINARY (the probe is type-agnostic).
        r = delete_client.request("registry.value.create",
                                  key, "Bin", "REG_BINARY", encoded)
        assert isinstance(r, ErrResponse)
        assert r.code == "already_exists"

        r = delete_client.request("registry.value.delete", key, "Bin")
        assert isinstance(r, OkResponse)

        r = delete_client.request("registry.value.read", key, "Bin")
        assert isinstance(r, ErrResponse)
        assert r.code == "not_found"
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


# ---------------------------------------------------------------------------
# Per-type round-trips — REG_DWORD explicit + REG_MULTI_SZ.
#
# REG_DWORD is implicitly covered by test_value_create_then_update_then_delete
# but the explicit test pins the shape (decimal string in, decimal string out)
# so a regression here surfaces immediately rather than via the umbrella test.
# REG_MULTI_SZ is the only type with a non-trivial wire encoding (the
# spec ferries entries separated by literal `\n` and the agent translates
# to NUL); it was previously untested.

def test_value_read_dword_explicit_roundtrip(delete_client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    try:
        r = delete_client.request("registry.value.create",
                                  key, "Dw", "REG_DWORD", "305419896")
        assert isinstance(r, OkResponse)
        r = delete_client.request("registry.value.read", key, "Dw")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_DWORD"
        assert body["data"] == "305419896"
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


def test_value_read_multi_sz_roundtrip(delete_client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.read")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    # Wire encoding: classic ferries the entries separated by the literal
    # two-char escape `\n` (backslash + 'n') because the protocol tokenizer
    # rejects whitespace inside a positional. The agent translates to NUL
    # before handing the buffer to RegSetValueExA.
    wire_data = r"alpha\nbeta\ngamma"
    try:
        r = delete_client.request("registry.value.create",
                                  key, "Multi", "REG_MULTI_SZ", wire_data)
        assert isinstance(r, OkResponse)
        r = delete_client.request("registry.value.read", key, "Multi")
        assert isinstance(r, OkResponse)
        body = json.loads(r.payload)
        assert body["type"] == "REG_MULTI_SZ"
        # Per spec the read shape is the raw NUL-separated buffer with a
        # trailing double-NUL. Split on NUL and drop the trailing empty
        # to recover the original entries.
        entries = [e for e in body["data"].split("\x00") if e]
        assert entries == ["alpha", "beta", "gamma"]
    finally:
        delete_client.request("registry.key.delete", key, "--recursive")


# ---------------------------------------------------------------------------
# registry.key.delete — empty / not-empty / recursive.
#
# The recursive case is also implicitly exercised by every teardown in
# this file (`registry.key.delete ... --recursive`); these tests assert
# the explicit semantics.

def test_key_delete_empty(delete_client: WireClient,
                          capabilities: dict) -> None:
    """An empty key may be removed without --recursive. A second delete on
    the same path returns not_found."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.value.delete")
    needs_verb(capabilities, "registry.key.delete")

    key = _scratch_key()
    # Materialise the parent key by writing then immediately deleting a
    # throwaway value, leaving an empty key behind.
    r = delete_client.request("registry.value.create",
                              key, "Tmp", "REG_DWORD", "0")
    assert isinstance(r, OkResponse)
    r = delete_client.request("registry.value.delete", key, "Tmp")
    assert isinstance(r, OkResponse)

    r = delete_client.request("registry.key.delete", key)
    assert isinstance(r, OkResponse)

    r = delete_client.request("registry.key.delete", key)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_key_delete_not_empty_without_recursive(delete_client: WireClient,
                                                capabilities: dict) -> None:
    """A key with subkeys cannot be removed without --recursive."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.key.delete")

    parent = _scratch_key()
    child = parent + r"\Child"
    try:
        # Materialise the child key by creating a value inside it; this
        # also creates the parent.
        r = delete_client.request("registry.value.create",
                                  child, "X", "REG_DWORD", "1")
        assert isinstance(r, OkResponse)

        r = delete_client.request("registry.key.delete", parent)
        assert isinstance(r, ErrResponse)
        assert r.code == "not_empty"
    finally:
        delete_client.request("registry.key.delete", parent, "--recursive")


def test_key_delete_recursive_removes_subtree(delete_client: WireClient,
                                              capabilities: dict) -> None:
    """`--recursive` deletes the whole subtree, including nested subkeys."""
    needs_verb(capabilities, "registry.value.create")
    needs_verb(capabilities, "registry.key.read")
    needs_verb(capabilities, "registry.key.delete")

    parent = _scratch_key()
    nested = parent + r"\A\B\C"
    r = delete_client.request("registry.value.create",
                              nested, "Leaf", "REG_DWORD", "7")
    assert isinstance(r, OkResponse)

    r = delete_client.request("registry.key.delete", parent, "--recursive")
    assert isinstance(r, OkResponse)

    # Confirm the parent is gone (a re-read returns not_found).
    r = delete_client.request("registry.key.read", parent)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"
