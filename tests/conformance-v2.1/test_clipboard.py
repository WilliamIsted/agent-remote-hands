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


def test_clipboard_get_at_read_tier(client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    r = client.request("clipboard.get")
    assert isinstance(r, OkResponse)
    # Empty clipboard → empty payload; either way, just bytes.


def test_clipboard_set_requires_update_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.set")
    r = client.request("clipboard.set", "5", payload=b"hello")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_clipboard_round_trip_at_update_tier(update_client: WireClient,
                                             capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    payload = "agent-remote-hands conformance test".encode("utf-8")
    r = update_client.request("clipboard.set",
                              str(len(payload)), payload=payload)
    assert isinstance(r, OkResponse)
    r = update_client.request("clipboard.get")
    assert isinstance(r, OkResponse)
    assert r.payload == payload
