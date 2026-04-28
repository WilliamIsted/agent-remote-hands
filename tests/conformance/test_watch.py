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

"""Tests for `watch.*` subscription-based verbs.

These exercise the registration path; verifying actual EVENT delivery is
left to integration scenarios with deliberate triggers (process spawn-and-die,
file create-and-delete, etc.) which the harness here does not orchestrate."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_watch_window_returns_subscription_id(client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "watch.window")
    r = client.request("watch.window", "--title-prefix", "Conformance")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    # Tear down so the worker thread stops.
    client.request("watch.cancel", body["subscription_id"])


def test_watch_window_requires_title_prefix(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "watch.window")
    r = client.request("watch.window")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_watch_cancel_is_idempotent(client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "watch.cancel")
    r = client.request("watch.cancel", "sub:never-existed")
    assert isinstance(r, OkResponse)


def test_watch_process_returns_subscription_id(client: WireClient,
                                               capabilities: dict) -> None:
    needs_verb(capabilities, "watch.process")
    # PID 4 is System on Windows, virtually always alive. We just verify the
    # subscription registers successfully; we cancel it before its thread
    # ever delivers an event.
    r = client.request("watch.process", "4")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])
