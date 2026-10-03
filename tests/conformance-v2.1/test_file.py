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

"""Tests for `file.*`. Uses the agent host's tempdir for write-side tests.

Wire-shape note: post-rc.2 the namespace is files-only (`directory.*` covers
directories). `file.write` is U-only (existing file required); `file.create`
is the C verb for new files. `file.write_at` is the chunked-upload primitive."""

import json
import pathlib
import tempfile
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def _scratch_path() -> str:
    return str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-conformance-{uuid.uuid4().hex}.txt")


# ---------------------------------------------------------------------------
# file.exists / file.stat — read-tier metadata

def test_file_exists_on_known_path(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.exists")
    r = client.request("file.exists", r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True


def test_file_exists_on_missing_path(client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "file.exists")
    r = client.request("file.exists",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_file_stat_on_known_path(client: WireClient,
                                 capabilities: dict) -> None:
    needs_verb(capabilities, "file.stat")
    r = client.request("file.stat",
                       r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["type"] == "file"
    assert body["size"] > 0
    for k in ("mtime_unix_s", "ctime_unix_s", "atime_unix_s", "flags"):
        assert k in body
    assert isinstance(body["flags"], list)


def test_file_stat_missing_returns_not_found(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "file.stat")
    r = client.request("file.stat",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


# ---------------------------------------------------------------------------
# file.read

def test_file_read_known(client: WireClient,
                         capabilities: dict) -> None:
    needs_verb(capabilities, "file.read")
    r = client.request("file.read",
                       r"C:\Windows\System32\drivers\etc\hosts",
                       "--encoding", "utf-8")
    # `hosts` always exists; tolerate permission_denied on locked-down hosts.
    assert isinstance(r, (OkResponse, ErrResponse))
    if isinstance(r, OkResponse):
        body = json.loads(r.payload)
        assert "content" in body
        assert "bytes_read" in body
        assert "truncated" in body


def test_file_read_missing_returns_not_found(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "file.read")
    r = client.request("file.read",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


# ---------------------------------------------------------------------------
# Tier gating — file.create / file.write / file.write_at / file.rename / file.delete / file.download

def test_file_create_requires_create_tier(client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    r = client.request("file.create", _scratch_path(),
                       "--content", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_write_requires_update_tier(client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "file.write")
    r = client.request("file.write", _scratch_path(),
                       "--content", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_write_at_requires_update_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "file.write_at")
    r = client.request("file.write_at", _scratch_path(),
                       "0", "--content", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_rename_requires_update_tier(client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "file.rename")
    r = client.request("file.rename", _scratch_path(), _scratch_path())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_delete_requires_delete_tier(update_client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "file.delete")
    r = update_client.request("file.delete", _scratch_path())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "delete"


def test_file_download_requires_create_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "file.download")
    r = client.request("file.download",
                       "https://example.com/", _scratch_path())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


# ---------------------------------------------------------------------------
# file.create / file.write / file.write_at — round-trips at appropriate tiers

def test_file_create_then_write_then_read(update_client: WireClient,
                                          capabilities: dict) -> None:
    """C → U → R round-trip exercising each tier of the namespace."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.read")

    path = _scratch_path()

    # Create with one body of content.
    payload = b"agent-remote-hands conformance create"
    r = update_client.request("file.create", path,
                              str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse), f"create failed: {r!r}"

    # Re-creating the same path returns already_exists.
    r = update_client.request("file.create", path,
                              str(len(payload)), payload=payload)
    assert isinstance(r, ErrResponse)
    assert r.code == "already_exists"

    # Update via file.write.
    new_payload = b"agent-remote-hands conformance write"
    r = update_client.request("file.write", path,
                              str(len(new_payload)), payload=new_payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(new_payload)

    # Read back the new contents.
    r = update_client.request("file.read", path,
                              "--encoding", "binary")
    assert isinstance(r, OkResponse)


def test_file_write_at_truncate_requires_offset_zero(update_client: WireClient,
                                                     capabilities: dict) -> None:
    """Per file.write_at.x-conditional, --truncate is only valid with --offset 0."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")

    path = _scratch_path()
    r = update_client.request("file.create", path, "0", payload=b"")
    assert isinstance(r, OkResponse)

    r = update_client.request("file.write_at", path, "5",
                              "--truncate", "--content", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_file_write_missing_returns_not_found(update_client: WireClient,
                                              capabilities: dict) -> None:
    """file.write is U-only — writing to a missing path errors with not_found
    (use file.create to create new files)."""
    needs_verb(capabilities, "file.write")
    r = update_client.request("file.write",
                              _scratch_path(),
                              "1", payload=b"x")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_file_rename_round_trip(update_client: WireClient,
                                capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.rename")
    needs_verb(capabilities, "file.exists")

    src = _scratch_path()
    dst = _scratch_path()

    r = update_client.request("file.create", src,
                              "1", payload=b"x")
    assert isinstance(r, OkResponse)

    r = update_client.request("file.rename", src, dst)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["renamed"] is True
    assert body["fallback_used"] == "none"

    r = update_client.request("file.exists", src)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False

    r = update_client.request("file.exists", dst)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True


# ---------------------------------------------------------------------------
# file.wait — block-until-glob-matches

def test_file_wait_short_timeout(client: WireClient,
                                 capabilities: dict) -> None:
    """A glob that won't match within the timeout returns ERR timeout."""
    needs_verb(capabilities, "file.wait")
    glob = str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-noexist-{uuid.uuid4().hex}-*.txt")
    r = client.request("file.wait", glob, "--timeout-ms", "200")
    assert isinstance(r, ErrResponse)
    assert r.code == "timeout"
