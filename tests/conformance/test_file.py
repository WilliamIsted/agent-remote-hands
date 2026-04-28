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

"""Tests for `file.*`. Uses the temporary directory of the agent host's user
profile for write-side tests."""

import json
import pathlib
import tempfile
import uuid

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def _scratch_path() -> str:
    return str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-conformance-{uuid.uuid4().hex}.txt")


def test_file_exists_on_known_path(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.exists")
    r = client.request("file.exists", r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exists"] is True


def test_file_exists_on_missing_path(client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "file.exists")
    r = client.request("file.exists",
                       r"C:\definitely-not-there-" + uuid.uuid4().hex)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exists"] is False


def test_file_list_on_directory(client: WireClient,
                                capabilities: dict) -> None:
    needs_verb(capabilities, "file.list")
    r = client.request("file.list", r"C:\Windows")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "entries" in body
    assert isinstance(body["entries"], list)


def test_file_write_requires_drive_tier(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "file.write")
    r = client.request("file.write", _scratch_path(), "5", payload=b"hello")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_round_trip(drive_client: WireClient,
                         capabilities: dict) -> None:
    needs_verb(capabilities, "file.write")
    needs_verb(capabilities, "file.read")
    needs_verb(capabilities, "file.delete")

    path = _scratch_path()
    payload = b"agent-remote-hands conformance round-trip"

    # write
    r = drive_client.request("file.write", path, str(len(payload)),
                             payload=payload)
    assert isinstance(r, OkResponse)

    # read back
    r = drive_client.request("file.read", path)
    assert isinstance(r, OkResponse)
    assert r.payload == payload

    # cleanup needs power tier; drive tier won't suffice. Skip cleanup but
    # leave a TODO marker — temp files don't accumulate badly.


def test_file_delete_requires_power_tier(drive_client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "file.delete")
    r = drive_client.request("file.delete", _scratch_path())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "power"
