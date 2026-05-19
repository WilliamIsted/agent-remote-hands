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

"""Tests for `directory.*`. Verb names and shapes per the post-rc.2 spec —
in particular `directory.delete` (was `directory.remove` pre-rc.2) and the
dropped `removed: true` field on the response."""

import json
import pathlib
import tempfile
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def _scratch_dir() -> str:
    return str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-conformance-dir-{uuid.uuid4().hex}")


# ---------------------------------------------------------------------------
# Read-tier verbs

def test_directory_list_on_known_path(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "directory.list")
    r = client.request("directory.list", r"C:\Windows")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "entries" in body
    assert isinstance(body["entries"], list)
    # System32 etc. should be in there.
    assert len(body["entries"]) > 0


def test_directory_list_entries_have_required_fields(client: WireClient,
                                                     capabilities: dict) -> None:
    needs_verb(capabilities, "directory.list")
    r = client.request("directory.list", r"C:\Windows", "--limit", "5")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)["entries"]
    for e in entries:
        for k in ("name", "type", "size", "mtime_unix_s",
                  "ctime_unix_s", "atime_unix_s", "flags"):
            assert k in e, f"directory.list entry missing {k}: {e}"
        assert e["type"] in {"file", "directory", "link", "other"}
        assert isinstance(e["flags"], list)


def test_directory_list_pattern_filter(client: WireClient,
                                       capabilities: dict) -> None:
    """Glob-style pattern filtering."""
    needs_verb(capabilities, "directory.list")
    r = client.request("directory.list", r"C:\Windows",
                       "--pattern", "System*")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)["entries"]
    for e in entries:
        # Case-insensitive — 'System' matches 'System32', 'system.ini', etc.
        assert e["name"].lower().startswith("system"), \
            f"pattern filter leaked: {e['name']}"


def test_directory_list_limit_caps_result(client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "directory.list")
    r = client.request("directory.list", r"C:\Windows", "--limit", "3")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)["entries"]
    assert len(entries) <= 3


def test_directory_stat_on_known_path(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "directory.stat")
    r = client.request("directory.stat", r"C:\Windows")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["type"] == "directory"
    assert isinstance(body["entry_count"], int)
    assert body["entry_count"] > 0
    assert isinstance(body["mtime_unix_s"], int)
    assert isinstance(body["flags"], list)


def test_directory_stat_rejects_files(client: WireClient,
                                      capabilities: dict) -> None:
    """Stat'ing a file path returns ERR not_a_directory rather than a stat shape."""
    needs_verb(capabilities, "directory.stat")
    r = client.request("directory.stat",
                       r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_a_directory"


def test_directory_exists_on_known_dir(client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists", r"C:\Windows")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True


def test_directory_exists_false_for_files(client: WireClient,
                                          capabilities: dict) -> None:
    """A path that exists but is a file returns exists:false."""
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists",
                       r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_directory_exists_false_for_missing(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


# ---------------------------------------------------------------------------
# Tier gating

def test_directory_create_requires_create_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    r = client.request("directory.create", _scratch_dir())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_directory_rename_requires_update_tier(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "directory.rename")
    r = client.request("directory.rename",
                       _scratch_dir(), _scratch_dir())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_directory_delete_requires_delete_tier(update_client: WireClient,
                                               capabilities: dict) -> None:
    """`directory.delete` (was `directory.remove` pre-rc.2) needs delete tier
    — update tier shouldn't suffice."""
    needs_verb(capabilities, "directory.delete")
    r = update_client.request("directory.delete", _scratch_dir())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "delete"


# ---------------------------------------------------------------------------
# Round-trips

def test_directory_create_round_trip(create_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.list")

    path = _scratch_dir()
    r = create_client.request("directory.create", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["created"] is True

    r = create_client.request("directory.list", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["entries"] == []


def test_directory_create_with_parents(create_client: WireClient,
                                       capabilities: dict) -> None:
    """`--parents` lets us create nested missing components in one call."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.exists")

    parent = _scratch_dir()
    nested = parent + "\\a\\b\\c"
    r = create_client.request("directory.create", nested, "--parents")
    assert isinstance(r, OkResponse)

    r = create_client.request("directory.exists", nested)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True


def test_directory_create_then_delete(delete_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.exists")
    needs_verb(capabilities, "directory.delete")

    path = _scratch_dir()
    r = delete_client.request("directory.create", path)
    assert isinstance(r, OkResponse)

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True

    r = delete_client.request("directory.delete", path)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    # `removed: true` was dropped pre-rc.2 — only entries_removed remains.
    assert body["entries_removed"] == 0

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_directory_delete_non_empty_requires_recursive(
        delete_client: WireClient, capabilities: dict) -> None:
    """Without --recursive, deleting a non-empty directory returns ERR not_empty."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.delete")
    needs_verb(capabilities, "file.write")

    parent = _scratch_dir()
    r = delete_client.request("directory.create", parent)
    assert isinstance(r, OkResponse)

    inside = parent + "\\inner.txt"
    payload = b"x"
    r = delete_client.request("file.write", inside,
                              str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse)

    # Non-recursive delete must fail with not_empty.
    r = delete_client.request("directory.delete", parent)
    assert isinstance(r, ErrResponse)
    assert r.code == "not_empty"

    # Recursive delete succeeds and reports a positive entries_removed.
    r = delete_client.request("directory.delete", parent, "--recursive")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["entries_removed"] >= 1


def test_directory_rename_round_trip(update_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.rename")
    needs_verb(capabilities, "directory.exists")

    src = _scratch_dir()
    dst = _scratch_dir()
    r = update_client.request("directory.create", src)
    assert isinstance(r, OkResponse)

    r = update_client.request("directory.rename", src, dst)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["renamed"] is True
    assert body["fallback_used"] == "none"

    r = update_client.request("directory.exists", src)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False

    r = update_client.request("directory.exists", dst)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True


# ---------------------------------------------------------------------------
# Header-quoting (PROTOCOL.md §1.2.5)

def test_directory_path_with_spaces(delete_client: WireClient,
                                    capabilities: dict) -> None:
    """A path with spaces round-trips end-to-end through the wire's
    double-quote grouping. The WireClient auto-quotes; the agent's tokeniser
    strips quotes and dispatches with the space-bearing path intact."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.exists")
    needs_verb(capabilities, "directory.delete")

    path = _scratch_dir() + " with spaces"

    r = delete_client.request("directory.create", path)
    assert isinstance(r, OkResponse)

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True

    r = delete_client.request("directory.delete", path)
    assert isinstance(r, OkResponse)


def test_directory_rename_paths_with_spaces(update_client: WireClient,
                                            capabilities: dict) -> None:
    """Two-positional verb with spaces in both args."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.rename")
    needs_verb(capabilities, "directory.exists")

    src = _scratch_dir() + " src dir"
    dst = _scratch_dir() + " dst dir"

    r = update_client.request("directory.create", src)
    assert isinstance(r, OkResponse)

    r = update_client.request("directory.rename", src, dst)
    assert isinstance(r, OkResponse)

    r = update_client.request("directory.exists", src)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False
    r = update_client.request("directory.exists", dst)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True
