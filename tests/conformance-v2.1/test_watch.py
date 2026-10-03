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
import pathlib
import tempfile
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# watch.cancel — idempotent

def test_watch_cancel_is_idempotent(client: WireClient,
                                    capabilities: dict) -> None:
    """Per the watch.cancel spec, cancellation of an unknown sub returns OK 0."""
    needs_verb(capabilities, "watch.cancel")
    r = client.request("watch.cancel", "sub:never-existed")
    assert isinstance(r, OkResponse)


# ---------------------------------------------------------------------------
# watch.window — title-prefix subscription

def test_watch_window_returns_subscription_id(client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "watch.window")
    r = client.request("watch.window", "--title-prefix", "Conformance")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])


def test_watch_window_requires_title_prefix(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "watch.window")
    r = client.request("watch.window")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# watch.process — pid-based subscription

def test_watch_process_returns_subscription_id(client: WireClient,
                                               capabilities: dict) -> None:
    """PID 4 is System on Windows; virtually always alive. We verify the
    subscription registers; we cancel before its thread delivers an event."""
    needs_verb(capabilities, "watch.process")
    r = client.request("watch.process", "4")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])


# ---------------------------------------------------------------------------
# watch.region — screen-region change subscription

def test_watch_region_returns_subscription_id(client: WireClient,
                                              capabilities: dict) -> None:
    needs_verb(capabilities, "watch.region")
    r = client.request("watch.region", "0,0,100,100",
                       "--encoding", "base64")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])


# ---------------------------------------------------------------------------
# watch.element — UIA-element-invalidation subscription

def test_watch_element_invalid_handle(client: WireClient,
                                      capabilities: dict) -> None:
    """A bogus element handle is target_gone (or invalid_args / uia_blind)."""
    needs_verb(capabilities, "watch.element")
    r = client.request("watch.element", "elt:99999")
    assert isinstance(r, ErrResponse)
    assert r.code in ("target_gone", "invalid_args", "uia_blind")


# ---------------------------------------------------------------------------
# watch.file — glob subscription

def test_watch_file_returns_subscription_id(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "watch.file")
    glob = str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-watch-{uuid.uuid4().hex}-*.txt")
    r = client.request("watch.file", glob)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])


def test_watch_file_recursive_flag_accepted(client: WireClient,
                                            capabilities: dict) -> None:
    """`--recursive` enables subtree watch; should be accepted by arg parsing."""
    needs_verb(capabilities, "watch.file")
    glob = str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-watch-{uuid.uuid4().hex}-*.txt")
    r = client.request("watch.file", glob, "--recursive")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    client.request("watch.cancel", body["subscription_id"])


# ---------------------------------------------------------------------------
# watch.registry — subscription mode + sync until_change one-shot

def test_watch_registry_subscription_mode(client: WireClient,
                                          capabilities: dict) -> None:
    """Default subscription-mode call returns a sub id immediately."""
    needs_verb(capabilities, "watch.registry")
    r = client.request("watch.registry",
                       r"HKCU\Software\Microsoft\Windows\CurrentVersion")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["subscription_id"].startswith("sub:")
    client.request("watch.cancel", body["subscription_id"])


def test_watch_registry_until_change_short_timeout(client: WireClient,
                                                   capabilities: dict) -> None:
    """`--until-change` with a tiny timeout returns ERR timeout when no change
    fires in that window — exercises the synchronous one-shot wait path."""
    needs_verb(capabilities, "watch.registry")
    # Use a key that is unlikely to mutate during the conformance run.
    r = client.request("watch.registry",
                       r"HKCU\Software\AgentRemoteHandsConformance-Watch-"
                       + uuid.uuid4().hex,
                       "--until-change", "--timeout-ms", "200")
    # Either ERR not_found (the key may not exist) or ERR timeout.
    assert isinstance(r, ErrResponse)
    assert r.code in ("timeout", "not_found")
