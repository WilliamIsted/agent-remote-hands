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

"""Tests for the MCP tool surface: tier filtering, dispatch, schema shape,
and a few representative handler round-trips against the mock agent."""

from __future__ import annotations

import json

import pytest
from agent_client import AgentClient
from tools import TOOLS, find_tool, tools_by_tier


# ---------------------------------------------------------------------------
# Tool registry shape

def test_no_duplicate_tool_names() -> None:
    names = [t.name for t in TOOLS]
    assert len(names) == len(set(names)), f"duplicate tool name in TOOLS: {names}"


def test_every_tool_has_valid_tier() -> None:
    valid = {"always", "observe", "drive", "power"}
    for t in TOOLS:
        assert t.tier in valid, f"{t.name}: bad tier {t.tier!r}"


def test_every_tool_serialises_to_mcp_shape() -> None:
    for t in TOOLS:
        d = t.to_mcp_tool()
        assert d["name"] == t.name
        assert d["description"]
        assert d["inputSchema"]["type"] == "object"
        assert "annotations" in d
        ann = d["annotations"]
        assert "readOnlyHint" in ann
        assert "destructiveHint" in ann


def test_destructive_tools_carry_destructive_hint() -> None:
    """Power-tier tools that delete / kill should advertise destructiveHint
    so MCP clients can prompt before invoking."""
    for t in TOOLS:
        if t.name in {"kill_process", "delete_file"}:
            assert t.destructive_hint, f"{t.name}: missing destructiveHint"


# ---------------------------------------------------------------------------
# Tier filtering

def test_observe_tier_sees_always_plus_observe_tools() -> None:
    observe = tools_by_tier("observe")
    names = {t.name for t in observe}
    # Always-available tools must appear.
    for required in ("agent_info", "request_drive_access", "request_power_access"):
        assert required in names, f"{required} missing from observe-tier surface"
    # No drive- or power-tier tools.
    for t in observe:
        assert t.tier in {"always", "observe"}


def test_drive_tier_includes_observe_and_drive() -> None:
    drive = {t.name for t in tools_by_tier("drive")}
    observe = {t.name for t in tools_by_tier("observe")}
    assert observe.issubset(drive), "drive surface must be a superset of observe"
    # Specific drive tools should appear.
    for required in ("click_element", "type_text", "launch"):
        assert required in drive


def test_power_tier_includes_everything() -> None:
    power = {t.name for t in tools_by_tier("power")}
    drive = {t.name for t in tools_by_tier("drive")}
    assert drive.issubset(power)
    for required in ("kill_process", "delete_file"):
        assert required in power


def test_find_tool_round_trip() -> None:
    for t in TOOLS:
        assert find_tool(t.name) is t
    assert find_tool("does-not-exist") is None


# ---------------------------------------------------------------------------
# Handler smoke tests against the mock agent

def test_agent_info_handler_returns_stub_json(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("agent_info")
        out = tool.handler({}, c)
        body = json.loads(out)
        assert body["protocol"] == "2.0"
    finally:
        c.close()


def test_request_drive_access_elevates_then_lists_more_tools(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        # Pre-elevation surface is observe + always.
        before = {t.name for t in tools_by_tier(c.current_tier)}
        assert "click_element" not in before

        request_drive = find_tool("request_drive_access")
        out = request_drive.handler({"reason": "smoke test"}, c)
        assert "Elevated to drive tier" in out
        assert c.current_tier == "drive"

        # Post-elevation surface includes drive tools.
        after = {t.name for t in tools_by_tier(c.current_tier)}
        assert "click_element" in after
        assert before.issubset(after)
    finally:
        c.close()


def test_request_drive_access_requires_reason(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        request_drive = find_tool("request_drive_access")
        with pytest.raises(ValueError):
            request_drive.handler({}, c)
        with pytest.raises(ValueError):
            request_drive.handler({"reason": "  "}, c)
    finally:
        c.close()


def test_request_drive_access_without_token_is_clear_error(mock_agent) -> None:
    """No token = no elevation. The handler should raise a RuntimeError with
    a message that points at the env var to set."""
    import os
    os.environ.pop("REMOTE_HANDS_TOKEN_PATH", None)
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    # Deliberately do NOT set a token.
    try:
        request_drive = find_tool("request_drive_access")
        with pytest.raises(RuntimeError) as excinfo:
            request_drive.handler({"reason": "smoke test"}, c)
        assert "token" in str(excinfo.value).lower()
    finally:
        c.close()


def test_request_power_access_requires_drive_first(mock_agent) -> None:
    """Power tier sits above drive. The handler should raise drive on the way."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        request_power = find_tool("request_power_access")
        out = request_power.handler({"reason": "shutdown the test box"}, c)
        assert "power tier" in out
        assert c.current_tier == "power"
    finally:
        c.close()
