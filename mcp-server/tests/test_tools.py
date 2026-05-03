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
    valid = {"always", "read", "create", "update", "delete", "extra_risky"}
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
    """delete/extra_risky tools that delete / kill should advertise
    destructiveHint so MCP clients can prompt before invoking."""
    for t in TOOLS:
        if t.name in {"process.kill", "file.delete"}:
            assert t.destructive_hint, f"{t.name}: missing destructiveHint"


# ---------------------------------------------------------------------------
# Tier filtering

def test_read_tier_sees_always_plus_read_tools() -> None:
    surface = tools_by_tier("read")
    names = {t.name for t in surface}
    # Always-available tools must appear.
    for required in ("system.info",
                     "request_create_access",
                     "request_update_access",
                     "request_delete_access",
                     "request_extra_risky_access"):
        assert required in names, f"{required} missing from read-tier surface"
    # No higher-tier tools.
    for t in surface:
        assert t.tier in {"always", "read"}, \
            f"{t.name} tier={t.tier!r} should not appear at read tier"


def test_update_tier_includes_read_create_update() -> None:
    """Holding `update` subsumes `create` + `read` per the ladder."""
    update = {t.name for t in tools_by_tier("update")}
    read = {t.name for t in tools_by_tier("read")}
    assert read.issubset(update), "update surface must be a superset of read"
    # Specific update tools should appear.
    for required in ("element.click", "input.type", "input.click", "file.write"):
        assert required in update


def test_delete_tier_subsumes_update() -> None:
    delete = {t.name for t in tools_by_tier("delete")}
    update = {t.name for t in tools_by_tier("update")}
    assert update.issubset(delete)
    for required in ("process.kill", "file.delete"):
        assert required in delete


def test_extra_risky_tier_includes_everything() -> None:
    extra = {t.name for t in tools_by_tier("extra_risky")}
    delete = {t.name for t in tools_by_tier("delete")}
    assert delete.issubset(extra)
    assert "system.power.cancel" in extra


def test_find_tool_round_trip() -> None:
    for t in TOOLS:
        assert find_tool(t.name) is t
    assert find_tool("does-not-exist") is None


# ---------------------------------------------------------------------------
# Spec-lifted tools

def test_lifted_tools_carry_wire_verb() -> None:
    """Tools constructed via `ToolDef.from_spec` populate `wire_verb`. Tools
    constructed via the inline `ToolDef(...)` constructor don't.

    Lifted tools share their wire-verb name as the MCP tool name (the rename
    that landed in v0.3.0 — there's no longer a translation layer between
    spec verb name and LLM-facing tool name)."""
    expected_lifts = {
        "system.info":     "system.info",
        "screen.capture":  "screen.capture",
        "window.list":     "window.list",
        "element.find":    "element.find",
        "file.read":       "file.read",
        "clipboard.get":   "clipboard.get",
        "input.click":     "input.click",
        "file.delete":     "file.delete",
    }
    for mcp_name, wire in expected_lifts.items():
        tool = find_tool(mcp_name)
        assert tool is not None, f"{mcp_name} not registered"
        assert tool.wire_verb == wire, \
            f"{mcp_name}: wire_verb is {tool.wire_verb!r}, expected {wire!r}"


def test_lifted_input_schemas_have_no_x_extensions() -> None:
    """The defensive strip_x_extensions pass at the from_spec boundary should
    leave no `x-*` keys in the LLM-facing input_schema."""
    def find_x(node, path=""):
        if isinstance(node, dict):
            for k, v in node.items():
                if k.startswith("x-"):
                    return f"{path}.{k}"
                hit = find_x(v, f"{path}.{k}")
                if hit:
                    return hit
        elif isinstance(node, list):
            for i, v in enumerate(node):
                hit = find_x(v, f"{path}[{i}]")
                if hit:
                    return hit
        return None

    for t in TOOLS:
        if t.wire_verb is None:
            continue  # inline tool, no spec lift
        leaked = find_x(t.input_schema)
        assert leaked is None, \
            f"{t.name}: leaked x-* extension at {leaked}"


# ---------------------------------------------------------------------------
# Handler smoke tests against the mock agent

def test_system_info_handler_returns_stub_json(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("system.info")
        out = tool.handler({}, c)
        body = json.loads(out)
        assert body["protocol"] == "2.1"
    finally:
        c.close()


def test_request_update_access_elevates_then_lists_more_tools(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        # Pre-elevation surface is read + always.
        before = {t.name for t in tools_by_tier(c.current_tier)}
        assert "element.click" not in before

        request_update = find_tool("request_update_access")
        out = request_update.handler({"reason": "smoke test"}, c)
        assert "Elevated to update tier" in out
        assert c.current_tier == "update"

        # Post-elevation surface includes update tools.
        after = {t.name for t in tools_by_tier(c.current_tier)}
        assert "element.click" in after
        assert before.issubset(after)
    finally:
        c.close()


def test_request_update_access_requires_reason(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        request_update = find_tool("request_update_access")
        with pytest.raises(ValueError):
            request_update.handler({}, c)
        with pytest.raises(ValueError):
            request_update.handler({"reason": "  "}, c)
    finally:
        c.close()


def test_request_update_access_without_token_is_clear_error(mock_agent) -> None:
    """No token = no elevation. The handler should raise a RuntimeError with
    a message that points at the env var to set."""
    import os
    os.environ.pop("REMOTE_HANDS_TOKEN_PATH", None)
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    # Deliberately do NOT set a token.
    try:
        request_update = find_tool("request_update_access")
        with pytest.raises(RuntimeError) as excinfo:
            request_update.handler({"reason": "smoke test"}, c)
        assert "token" in str(excinfo.value).lower()
    finally:
        c.close()


def test_request_extra_risky_access_top_of_ladder(mock_agent) -> None:
    """Raising directly to extra_risky should work in one call (the agent
    accepts the highest rung when the caller has the token)."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        request_extra = find_tool("request_extra_risky_access")
        out = request_extra.handler(
            {"reason": "shutdown the test box"}, c)
        assert "extra_risky tier" in out
        assert c.current_tier == "extra_risky"
    finally:
        c.close()


# ---------------------------------------------------------------------------
# Binary file-write tools (file.write_b64, file.upload)

def test_binary_write_tools_registered() -> None:
    """Both binary-capable wrappers around file.write must be registered
    at update tier. file.write (text) is the third member of the family."""
    names = {t.name for t in TOOLS}
    for required in ("file.write", "file.write_b64", "file.upload"):
        assert required in names, f"{required} missing from registry"

    update = {t.name for t in tools_by_tier("update")}
    for required in ("file.write", "file.write_b64", "file.upload"):
        assert required in update, f"{required} should be update-tier"


def test_file_write_b64_rejects_invalid_base64() -> None:
    """The b64 handler must validate before sending bytes over the wire."""
    tool = find_tool("file.write_b64")
    with pytest.raises(ValueError, match="not valid base64"):
        tool.handler({"path": "C:\\tmp\\x", "content_b64": "@@@not-b64@@@"}, None)


def test_file_upload_rejects_missing_source(tmp_path) -> None:
    """The upload handler must fail with a clear error if the source path
    on the controller doesn't exist, rather than sending an empty file or
    leaving the wire in a bad state."""
    tool = find_tool("file.upload")
    nonexistent = tmp_path / "definitely-not-here.bin"
    with pytest.raises(ValueError, match="could not read"):
        tool.handler(
            {"source_path": str(nonexistent),
             "destination_path": "C:\\tmp\\dest.bin"},
            None,
        )


# ---------------------------------------------------------------------------
# Subscription wait-fors (fire-once)

def test_wait_for_tools_registered() -> None:
    """All four fire-once wait tools should be registered, read-tier,
    and marked read-only."""
    expected = {
        "wait_for_visual_change", "wait_for_window",
        "wait_for_process_exit", "wait_for_file_change",
    }
    names = {t.name for t in TOOLS}
    for name in expected:
        assert name in names, f"{name} not registered"

    for name in expected:
        tool = find_tool(name)
        assert tool.tier == "read", f"{name} should be read-tier"
        assert tool.read_only_hint is True, f"{name} should advertise read_only_hint"


def test_wait_for_tools_in_read_surface() -> None:
    """Wait-for tools must appear in the read-tier surface so a fresh
    session can call them without elevation."""
    surface = {t.name for t in tools_by_tier("read")}
    for name in ("wait_for_visual_change", "wait_for_window",
                 "wait_for_process_exit", "wait_for_file_change"):
        assert name in surface
