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

"""windows-classic v2.1 conformance — assertions the vendored suite lacks.

ADDITIVE-ONLY. The other test files in this directory are a verbatim copy
of the protocol repo's `v2.1.0-rc.3` suite and are not hand-patched. This
file covers the classic v2.1 conformance fixes
(docs/classic-v2.1-conformance.md) that suite does not exercise:

  * Flag spelling: `--timeout-ms` (suite) and `--timeout_ms` (spec-derived
    callers) name the same property; boolean flags take `true` / `false`.
  * connection.* verbs advertised in system.capabilities.
  * Oversized header: ERR wire_desync, connection stays usable.
  * file.write_at to a missing path: not_found (never creates).
  * Spec tiers: power.lock extra_risky, power.cancel update.
  * file.* encodings, directory.list recursion naming, webp format code.

Every test is gated on `system.info.family == "windows-classic"`.
"""
from __future__ import annotations

import json
import pathlib
import socket
import tempfile
import uuid

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


@pytest.fixture(autouse=True)
def _classic_only(host: str, port: int) -> None:
    with WireClient(host, port) as c:
        c.hello()
        family = c.info().get("family")
    if family != "windows-classic":
        pytest.skip(f"classic-only test (agent family {family!r})")


def _scratch(suffix: str = ".txt") -> str:
    return str(pathlib.Path(tempfile.gettempdir()) /
               f"rh-classic-{uuid.uuid4().hex}{suffix}")


# ---------------------------------------------------------------------------
# Flag spelling + boolean values

def test_underscore_and_hyphen_flag_spellings_are_equivalent(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    a = client.request("window.list", "--include-monitor")
    b = client.request("window.list", "--include_monitor")
    assert isinstance(a, OkResponse) and isinstance(b, OkResponse)
    for body in (json.loads(a.payload), json.loads(b.payload)):
        for e in body[:3]:
            assert isinstance(e["monitor_index"], int)


def test_boolean_flag_accepts_explicit_false(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "window.list")
    visible = json.loads(client.request("window.list").payload)
    r = client.request("window.list", "--visible-only", "false")
    assert isinstance(r, OkResponse)
    everything = json.loads(r.payload)
    assert len(everything) >= len(visible)
    r = client.request("window.list", "--include-monitor", "false")
    assert isinstance(r, OkResponse)
    for e in json.loads(r.payload)[:3]:
        assert "monitor_index" not in e


def test_file_wait_accepts_snake_case_timeout(client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "file.wait")
    r = client.request("file.wait", _scratch("-*.none"), "--timeout_ms", "200")
    assert isinstance(r, ErrResponse)
    assert r.code == "timeout"


# ---------------------------------------------------------------------------
# connection.* advertisement + oversized-header resync

def test_connection_verbs_advertised(capabilities: dict) -> None:
    for verb in ("connection.hello", "connection.tier_raise",
                 "connection.tier_drop", "connection.reset",
                 "connection.close"):
        assert verb in capabilities, f"{verb} not advertised"


def test_oversized_header_returns_wire_desync_and_keeps_connection(
        host: str, port: int) -> None:
    s = socket.create_connection((host, port), timeout=10.0)
    try:
        f = s.makefile("rb")
        s.sendall(b"connection.hello conformance 2.1\n")
        assert f.readline().startswith(b"OK")
        s.sendall(b"system.health " + b"x" * 70000 + b"\n")
        line = f.readline().decode("ascii")
        assert line.startswith("ERR wire_desync"), line
        length = int(line.split()[2])
        f.read(length)
        s.sendall(b"system.health\n")
        assert f.readline().startswith(b"OK"), "connection not usable"
    finally:
        s.close()


# ---------------------------------------------------------------------------
# file.* — U verbs never create; encodings round-trip

def test_file_write_at_missing_returns_not_found(update_client: WireClient,
                                                 capabilities: dict) -> None:
    needs_verb(capabilities, "file.write_at")
    r = update_client.request("file.write_at", _scratch(), "0",
                              "1", payload=b"x")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_file_utf16le_round_trip(delete_client: WireClient,
                                 capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")
    path = _scratch()
    try:
        r = delete_client.request("file.create", path, "--content", "hé",
                                  "--encoding", "utf-16le")
        assert isinstance(r, OkResponse), r
        body = json.loads(r.payload)
        assert body == {"bytes_written": 4, "encoding": "utf-16le"}

        r = delete_client.request("file.read", path, "--encoding", "binary")
        raw = json.loads(r.payload)
        assert raw["content"] == "aADpAA=="          # 68 00 e9 00

        r = delete_client.request("file.read", path, "--encoding", "utf-16le")
        assert json.loads(r.payload)["content"] == "hé"
    finally:
        delete_client.request("file.delete", path)


def test_file_ascii_rejects_non_ascii(update_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    r = update_client.request("file.create", _scratch(), "--content",
                              "café", "--encoding", "ascii")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_file_write_at_reports_new_size(delete_client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    path = _scratch()
    try:
        r = delete_client.request("file.create", path, "3", payload=b"abc")
        assert isinstance(r, OkResponse)
        r = delete_client.request("file.write_at", path, "3", "2",
                                  payload=b"de")
        assert isinstance(r, OkResponse)
        assert json.loads(r.payload) == {"bytes_written": 2,
                                         "encoding": "utf-8", "new_size": 5}
    finally:
        delete_client.request("file.delete", path)


def test_file_exists_absent_shape(client: WireClient,
                                  capabilities: dict) -> None:
    needs_verb(capabilities, "file.exists")
    r = client.request("file.exists", _scratch())
    assert json.loads(r.payload) == {"exists": False, "type": "absent",
                                     "flags": []}


def test_directory_list_recursive_uses_forward_slashes(
        delete_client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.list")
    root = _scratch("")
    try:
        r = delete_client.request("directory.create", root + "\\a\\b",
                                  "--parents")
        assert isinstance(r, OkResponse)
        r = delete_client.request("directory.list", root, "--recursive")
        names = {e["name"] for e in json.loads(r.payload)["entries"]}
        assert names == {"a", "a/b"}
    finally:
        delete_client.request("directory.delete", root, "--recursive")


# ---------------------------------------------------------------------------
# Tiers + error codes

def test_power_cancel_is_update_tier(update_client: WireClient,
                                     capabilities: dict) -> None:
    """Spec x-crudx "U". The vendored suite's
    test_power_cancel_requires_extra_risky_tier is stale (ledgered)."""
    needs_verb(capabilities, "system.power.cancel")
    assert capabilities["system.power.cancel"]["tier"] == "update"
    r = update_client.request("system.power.cancel")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"          # nothing pending, but not gated


def test_power_lock_is_extra_risky_tier(capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.lock")
    assert capabilities["system.power.lock"]["tier"] == "extra_risky"


def test_screen_capture_webp_is_unsupported_format(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--format", "webp")
    assert isinstance(r, ErrResponse)
    assert r.code == "unsupported_format"


def test_window_find_regex_is_rejected(client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "window.find")
    r = client.request("window.find", "x", "--match", "regex")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"
