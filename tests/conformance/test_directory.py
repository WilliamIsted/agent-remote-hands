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

"""Tests for `directory.*`. New namespace in v2.1 — split out from `file.*`
plus a basic CRUDX-complete set of new directory primitives."""

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


def test_directory_stat_on_known_path(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "directory.stat")
    r = client.request("directory.stat", r"C:\Windows")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body.get("type") == "dir"
    assert isinstance(body.get("entry_count"), int)
    assert body["entry_count"] > 0


def test_directory_stat_rejects_files(client: WireClient,
                                      capabilities: dict) -> None:
    """stat on a file path should return an error, not a stat shape."""
    needs_verb(capabilities, "directory.stat")
    r = client.request("directory.stat",
                       r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, ErrResponse)


def test_directory_exists_on_known_dir(client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists", r"C:\Windows")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exists"] is True


def test_directory_exists_false_for_files(client: WireClient,
                                          capabilities: dict) -> None:
    """A path that exists but is a file returns exists:false from the
    directory namespace. Use file.exists for the polymorphic test."""
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists",
                       r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exists"] is False


def test_directory_exists_false_for_missing(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "directory.exists")
    r = client.request("directory.exists",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exists"] is False


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


def test_directory_remove_requires_delete_tier(update_client: WireClient,
                                               capabilities: dict) -> None:
    """directory.remove needs delete tier — update tier shouldn't suffice."""
    needs_verb(capabilities, "directory.remove")
    r = update_client.request("directory.remove", _scratch_dir())
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

    # The created directory should be listable and empty.
    r = create_client.request("directory.list", path)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["entries"] == []


def test_directory_create_then_remove(delete_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.exists")
    needs_verb(capabilities, "directory.remove")

    path = _scratch_dir()
    r = delete_client.request("directory.create", path)
    assert isinstance(r, OkResponse)

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True

    r = delete_client.request("directory.remove", path)
    assert isinstance(r, OkResponse)

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_directory_remove_non_empty_requires_recursive(
        delete_client: WireClient, capabilities: dict) -> None:
    """Without --recursive, removing a non-empty directory should fail."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.remove")
    needs_verb(capabilities, "file.write")

    parent = _scratch_dir()
    r = delete_client.request("directory.create", parent)
    assert isinstance(r, OkResponse)

    # Drop a file inside.
    inside = parent + "\\inner.txt"
    payload = b"x"
    r = delete_client.request("file.write", inside,
                              str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse)

    # Non-recursive remove must fail.
    r = delete_client.request("directory.remove", parent)
    assert isinstance(r, ErrResponse)

    # Recursive remove succeeds.
    r = delete_client.request("directory.remove", parent, "--recursive")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["removed"] is True
    assert body.get("entries_removed", 0) >= 1


def test_directory_rename_round_trip(update_client: WireClient,
                                     capabilities: dict) -> None:
    """Rename a directory; verify both old and new paths reflect the move."""
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
    """A path containing spaces round-trips end-to-end through the wire's
    double-quote grouping. The WireClient auto-quotes args containing
    spaces; the agent's tokeniser strips the quotes and dispatches with the
    space-bearing path intact."""
    needs_verb(capabilities, "directory.create")
    needs_verb(capabilities, "directory.exists")
    needs_verb(capabilities, "directory.remove")

    path = _scratch_dir() + " with spaces"

    r = delete_client.request("directory.create", path)
    assert isinstance(r, OkResponse)

    r = delete_client.request("directory.exists", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is True

    r = delete_client.request("directory.remove", path)
    assert isinstance(r, OkResponse)


def test_directory_rename_paths_with_spaces(update_client: WireClient,
                                            capabilities: dict) -> None:
    """Two-positional verb (directory.rename) with spaces in both args.
    Validates that the auto-quoting on the send side and the tokeniser on
    the agent side together preserve arg boundaries."""
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
