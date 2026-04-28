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

"""Tests for `clipboard.*`."""

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_clipboard_read_at_observe_tier(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.read")
    r = client.request("clipboard.read")
    assert isinstance(r, OkResponse)
    # Empty clipboard → empty payload; either way, just bytes.


def test_clipboard_write_requires_drive_tier(client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.write")
    r = client.request("clipboard.write", "5", payload=b"hello")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_clipboard_round_trip_at_drive_tier(drive_client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.read")
    needs_verb(capabilities, "clipboard.write")
    payload = "agent-remote-hands conformance test".encode("utf-8")
    r = drive_client.request("clipboard.write",
                             str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse)
    r = drive_client.request("clipboard.read")
    assert isinstance(r, OkResponse)
    assert r.payload == payload
