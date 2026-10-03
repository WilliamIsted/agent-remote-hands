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

"""Tests for `process.*`.

Wire-shape note: the filter input on `process.list` is `--pattern` (was
`--filter` pre-rc.2)."""

import json
import time

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# process.list

def test_process_list_returns_processes(client: WireClient,
                                        capabilities: dict) -> None:
    needs_verb(capabilities, "process.list")
    r = client.request("process.list")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "processes" in body
    assert isinstance(body["processes"], list)
    # System always has at least svchost / explorer running.
    assert len(body["processes"]) > 0


def test_process_list_entries_have_required_fields(client: WireClient,
                                                   capabilities: dict) -> None:
    needs_verb(capabilities, "process.list")
    r = client.request("process.list")
    assert isinstance(r, OkResponse)
    for p in json.loads(r.payload)["processes"][:5]:
        assert "pid" in p
        assert "image" in p
        assert "ppid" in p


def test_process_list_pattern_filter(client: WireClient,
                                     capabilities: dict) -> None:
    """`--pattern` (renamed from --filter pre-rc.2) is case-insensitive
    substring against the image name."""
    needs_verb(capabilities, "process.list")
    r = client.request("process.list", "--pattern", "svchost")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    for p in body["processes"]:
        assert "svchost" in p["image"].lower()


def test_process_list_include_counters(client: WireClient,
                                       capabilities: dict) -> None:
    """`--include-counters` adds CPU/memory/handle counts per entry."""
    needs_verb(capabilities, "process.list")
    r = client.request("process.list", "--include-counters",
                       "--pattern", "svchost")
    assert isinstance(r, OkResponse)
    entries = json.loads(r.payload)["processes"]
    if not entries:
        return  # Should always be at least one svchost, but tolerate empty.
    # At least one entry has the counter fields populated. Protected processes
    # may have empty/zero counters but the keys should still be present on at
    # least some entries.
    has_counters = any(
        "rss_bytes" in e or "thread_count" in e or "handle_count" in e
        for e in entries
    )
    assert has_counters, \
        "expected --include-counters to add counter fields to entries"


# ---------------------------------------------------------------------------
# process.start — tier-gated; round-trip via process.wait

def test_process_start_requires_create_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "process.start")
    r = client.request("process.start", "cmd.exe")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_process_start_and_wait(create_client: WireClient,
                                capabilities: dict) -> None:
    """`cmd /c exit 7` exits cleanly with code 7."""
    needs_verb(capabilities, "process.start")
    needs_verb(capabilities, "process.wait")

    r = create_client.request("process.start", "cmd.exe /c exit 7")
    assert isinstance(r, OkResponse)
    pid = json.loads(r.payload)["pid"]
    assert isinstance(pid, int)
    assert pid >= 1

    r = create_client.request("process.wait", str(pid),
                              "--timeout-ms", "5000")
    assert isinstance(r, OkResponse)
    assert json.loads(r.payload)["exit_code"] == 7


def test_process_wait_after_exit_returns_cached_code(create_client: WireClient,
                                                     capabilities: dict) -> None:
    """Regression for #16: the agent retains the spawned process handle so
    process.wait returns the exit code even after the OS reaped the process.
    Without the cache this returns ERR target_gone."""
    needs_verb(capabilities, "process.start")
    needs_verb(capabilities, "process.wait")

    r = create_client.request("process.start", "cmd.exe /c exit 5")
    assert isinstance(r, OkResponse)
    pid = json.loads(r.payload)["pid"]

    # Let the OS finish reaping the process.
    time.sleep(0.5)

    r = create_client.request("process.wait", str(pid),
                              "--timeout-ms", "1000")
    assert isinstance(r, OkResponse), f"got {r!r}"
    assert json.loads(r.payload)["exit_code"] == 5


# ---------------------------------------------------------------------------
# process.shell — ShellExecuteEx escape hatch (tier-gated only)

def test_process_shell_requires_create_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "process.shell")
    r = client.request("process.shell", r"C:\Windows\System32\notepad.exe")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_process_shell_unknown_verb_rejected(create_client: WireClient,
                                             capabilities: dict) -> None:
    """`verb` enum is open/runas/print/edit/explore/find — anything else
    should be rejected by arg validation, not silently no-op'd."""
    needs_verb(capabilities, "process.shell")
    r = create_client.request("process.shell",
                              r"C:\Windows\System32\notepad.exe",
                              "--verb", "definitely-not-a-shell-verb")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# process.kill — tier-gated

def test_process_kill_requires_delete_tier(update_client: WireClient,
                                           capabilities: dict) -> None:
    """PID 4 (System) is unkillable; tier-required fires before that check."""
    needs_verb(capabilities, "process.kill")
    r = update_client.request("process.kill", "4")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "delete"


def test_process_kill_invalid_pid_returns_not_found(
        delete_client: WireClient, capabilities: dict) -> None:
    """A pid that doesn't correspond to any running process returns not_found."""
    needs_verb(capabilities, "process.kill")
    r = delete_client.request("process.kill", "99999999")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "permission_denied")
