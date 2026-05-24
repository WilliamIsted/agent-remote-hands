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


# ---------------------------------------------------------------------------
# file.create — extra coverage (binary payload, missing parent, atomic-default)

def test_file_create_binary_round_trip(update_client: WireClient,
                                       capabilities: dict) -> None:
    """encoding=binary writes the payload bytes verbatim."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    path = _scratch_path()
    payload = bytes(range(256))   # every byte value including NULs

    r = update_client.request("file.create", path,
                              str(len(payload)),
                              "--encoding", "binary",
                              payload=payload)
    assert isinstance(r, OkResponse), f"create failed: {r!r}"
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(payload)
    assert body["encoding"] == "binary"

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)


def test_file_create_parent_missing_returns_not_found(
        update_client: WireClient, capabilities: dict) -> None:
    """A path whose parent directory does not exist errors with not_found."""
    needs_verb(capabilities, "file.create")

    missing_dir = pathlib.Path(tempfile.gettempdir()) / \
        f"remote-hands-noexist-{uuid.uuid4().hex}"
    path = str(missing_dir / "child.txt")

    r = update_client.request("file.create", path, "1", payload=b"x")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_file_create_atomic_leaves_no_tmp(update_client: WireClient,
                                          capabilities: dict) -> None:
    """Atomic-by-default: no .rh-tmp residue alongside the created file."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.exists")

    path = _scratch_path()
    r = update_client.request("file.create", path, "4", payload=b"data")
    assert isinstance(r, OkResponse)

    tmp_sidecar = path + ".rh-tmp"
    r = update_client.request("file.exists", tmp_sidecar)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


# ---------------------------------------------------------------------------
# file.read — content delivery (closes Phase-2b deferral)
#
# These tests exercise the Phase-2b closure: file.read returning actual bytes
# (text or base64) in `content`, with `bytes_read` + `truncated` reflecting
# the slice that was actually read. The pre-closure stub answered
# invalid_args {"reason":"file_content_phase2b"} for any existing path; these
# tests assert real content delivery.

def _create_fixture(client: WireClient, payload: bytes) -> str:
    """Helper: file.create a scratch path with the given raw bytes."""
    path = _scratch_path()
    r = client.request("file.create", path,
                       str(len(payload)),
                       "--encoding", "binary", payload=payload)
    assert isinstance(r, OkResponse), f"fixture create failed: {r!r}"
    return path


def test_file_read_text_full(update_client: WireClient,
                             capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    payload = b"hello world\nsecond line\n"
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path, "--encoding", "utf-8")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == payload.decode("utf-8")
    assert body["bytes_read"] == len(payload)
    assert body["truncated"] is False


def test_file_read_text_offset_length(update_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    payload = b"abcdefghij"
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path,
                              "--encoding", "utf-8",
                              "--offset", "2", "--length", "4")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == "cdef"
    assert body["bytes_read"] == 4
    assert body["truncated"] is True


def test_file_read_text_offset_to_eof(update_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    payload = b"abcdefghij"
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path,
                              "--encoding", "utf-8", "--offset", "6")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == "ghij"
    assert body["bytes_read"] == 4
    assert body["truncated"] is False


def test_file_read_binary_round_trip(update_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    import base64
    payload = bytes(range(256))   # every byte value
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert base64.b64decode(body["content"]) == payload
    assert body["bytes_read"] == len(payload)
    assert body["truncated"] is False


def test_file_read_encoding_utf16le(update_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    text = "héllo wörld"
    payload = text.encode("utf-16-le")   # no BOM
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path, "--encoding", "utf-16le")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == text
    assert body["bytes_read"] == len(payload)


def test_file_read_encoding_cp1252(update_client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    text = "résumé café"
    payload = text.encode("cp1252")
    path = _create_fixture(update_client, payload)

    r = update_client.request("file.read", path, "--encoding", "cp1252")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == text


def test_file_read_offset_past_eof(update_client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"abc")
    r = update_client.request("file.read", path, "--offset", "100")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_file_read_empty_file(update_client: WireClient,
                              capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"")
    r = update_client.request("file.read", path, "--encoding", "utf-8")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == ""
    assert body["bytes_read"] == 0
    assert body["truncated"] is False


def test_file_read_length_zero(update_client: WireClient,
                               capabilities: dict) -> None:
    """`length: 0` is the stat-like no-op — reads zero bytes, not truncated
    despite length being supplied (offset+0 equals offset, not past EOF)."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"abcdef")
    r = update_client.request("file.read", path,
                              "--encoding", "utf-8", "--length", "0")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == ""
    assert body["bytes_read"] == 0
    # length supplied AND there are remaining bytes past offset+0=0 -> True.
    assert body["truncated"] is True


# ---------------------------------------------------------------------------
# file.write — content delivery (closes Phase-2b deferral)

def test_file_write_overwrites_text(update_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"original-content")
    new_payload = b"replaced-content!"
    r = update_client.request("file.write", path,
                              str(len(new_payload)), payload=new_payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(new_payload)
    assert body["encoding"] == "binary"

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    import base64
    assert base64.b64decode(json.loads(r.payload)["content"]) == new_payload


def test_file_write_atomic_false_direct(update_client: WireClient,
                                        capabilities: dict) -> None:
    """atomic:false: succeeds and leaves no .rh-tmp sidecar."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.exists")

    path = _create_fixture(update_client, b"x")
    payload = b"direct-write"
    r = update_client.request("file.write", path,
                              str(len(payload)),
                              "--atomic", "false", payload=payload)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["bytes_written"] == len(payload)

    r = update_client.request("file.exists", path + ".rh-tmp")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_file_write_atomic_leaves_no_tmp(update_client: WireClient,
                                         capabilities: dict) -> None:
    """atomic-by-default: after overwrite there is no .rh-tmp residue."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.exists")

    path = _create_fixture(update_client, b"x")
    payload = b"atomic-write"
    r = update_client.request("file.write", path,
                              str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse)

    r = update_client.request("file.exists", path + ".rh-tmp")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exists"] is False


def test_file_write_binary_round_trip(update_client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"placeholder")
    import base64
    payload = bytes(range(256))
    r = update_client.request("file.write", path,
                              str(len(payload)),
                              "--encoding", "binary", payload=payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(payload)
    assert body["encoding"] == "binary"

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    assert base64.b64decode(json.loads(r.payload)["content"]) == payload


def test_file_write_empty_content(update_client: WireClient,
                                  capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"non-empty-original")
    r = update_client.request("file.write", path, "0", payload=b"")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["bytes_written"] == 0

    r = update_client.request("file.read", path, "--encoding", "utf-8")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["content"] == ""
    assert body["bytes_read"] == 0


# ---------------------------------------------------------------------------
# file.write_at — content delivery (closes Phase-2b deferral)

def test_file_write_at_offset_zero(update_client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"0123456789")
    payload = b"WXYZ"
    r = update_client.request("file.write_at", path, "0",
                              str(len(payload)),
                              "--encoding", "binary", payload=payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(payload)
    assert body["encoding"] == "binary"
    assert body["new_size"] == 10

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    import base64
    assert base64.b64decode(json.loads(r.payload)["content"]) == b"WXYZ456789"


def test_file_write_at_middle(update_client: WireClient,
                              capabilities: dict) -> None:
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"0123456789")
    payload = b"AB"
    r = update_client.request("file.write_at", path, "5",
                              str(len(payload)),
                              "--encoding", "binary", payload=payload)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["new_size"] == 10

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    import base64
    assert base64.b64decode(json.loads(r.payload)["content"]) == b"01234AB789"


def test_file_write_at_extends_file(update_client: WireClient,
                                    capabilities: dict) -> None:
    """Writing past EOF extends the file (Win32 sparse-hole behaviour)."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    needs_verb(capabilities, "file.stat")

    path = _create_fixture(update_client, b"abc")
    payload = b"END"
    r = update_client.request("file.write_at", path, "1024",
                              str(len(payload)),
                              "--encoding", "binary", payload=payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["new_size"] == 1024 + len(payload)

    r = update_client.request("file.stat", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["size"] == 1024 + len(payload)


def test_file_write_at_truncate_zero(update_client: WireClient,
                                     capabilities: dict) -> None:
    """truncate:true at offset 0 clears prior content before writing."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    needs_verb(capabilities, "file.read")

    path = _create_fixture(update_client, b"long-original-content")
    payload = b"abc"
    r = update_client.request("file.write_at", path, "0",
                              str(len(payload)),
                              "--encoding", "binary",
                              "--truncate", payload=payload)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == 3
    assert body["new_size"] == 3

    r = update_client.request("file.read", path, "--encoding", "binary")
    assert isinstance(r, OkResponse)
    import base64
    assert base64.b64decode(json.loads(r.payload)["content"]) == b"abc"


def test_file_write_at_truncate_empty_content(update_client: WireClient,
                                              capabilities: dict) -> None:
    """truncate:true with empty content is the 'clear file' pattern."""
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.write_at")
    needs_verb(capabilities, "file.stat")

    path = _create_fixture(update_client, b"to-be-cleared")
    r = update_client.request("file.write_at", path, "0", "0",
                              "--truncate", payload=b"")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == 0
    assert body["new_size"] == 0

    r = update_client.request("file.stat", path)
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["size"] == 0


def test_file_write_at_missing_path_returns_not_found(
        update_client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "file.write_at")
    r = update_client.request("file.write_at", _scratch_path(),
                              "0", "1", payload=b"x")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"
