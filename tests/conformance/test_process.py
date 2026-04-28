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

"""Tests for `process.*`."""

import json

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


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


def test_process_list_filter(client: WireClient,
                             capabilities: dict) -> None:
    needs_verb(capabilities, "process.list")
    r = client.request("process.list", "--filter", "svchost")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    for p in body["processes"]:
        assert "svchost" in p["image"].lower()


def test_process_start_requires_drive_tier(client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "process.start")
    r = client.request("process.start", "cmd.exe")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_process_kill_requires_power_tier(drive_client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "process.kill")
    # PID 0 (System Idle Process) is unkillable; verb should still report
    # tier-mismatch first since drive < power.
    r = drive_client.request("process.kill", "0")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_process_start_and_wait(drive_client: WireClient,
                                capabilities: dict) -> None:
    needs_verb(capabilities, "process.start")
    needs_verb(capabilities, "process.wait")
    # `cmd /c exit 7` exits cleanly with code 7.
    r = drive_client.request("process.start", "cmd.exe /c exit 7")
    assert isinstance(r, OkResponse)
    pid = json.loads(r.payload)["pid"]

    r = drive_client.request("process.wait", str(pid), "5000")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["exit_code"] == 7
