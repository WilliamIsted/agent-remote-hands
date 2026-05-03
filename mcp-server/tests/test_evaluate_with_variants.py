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

"""Tests for evaluate_with_variants and the variant-family registry.

The registry is structural data — tested for shape and consistency.
The handler is tested for: dispatch, ruled-out handling, coverage-gap
detection, and basic error paths."""

from __future__ import annotations

import json

import pytest
from agent_client import AgentClient
from tools import find_tool, TOOLS
from variant_families import (
    VARIANT_FAMILIES, FamilyEntry, has_family, lookup,
)


# ---------------------------------------------------------------------------
# Registry shape

def test_registry_entries_are_family_entry_instances() -> None:
    for k, v in VARIANT_FAMILIES.items():
        assert isinstance(v, FamilyEntry), f"{k}: not a FamilyEntry"


def test_registry_alternatives_match_family_lists() -> None:
    """For every tool in `family`, there should be an entry in
    `alternatives` so coverage-gap advisories can include reason text."""
    for name, entry in VARIANT_FAMILIES.items():
        for member in entry.family:
            assert member in entry.alternatives, (
                f"{name}: family lists {member!r} but no alternatives entry "
                f"with reason text. Either add a reason or remove from family."
            )


def test_registry_tools_are_real_mcp_tools() -> None:
    """Every tool referenced in the registry (as a key or as a family
    member) must be a registered MCP tool. Catches typos and stale
    references when tools are renamed."""
    registered = {t.name for t in TOOLS}
    for name, entry in VARIANT_FAMILIES.items():
        assert name in registered, (
            f"registry key {name!r} is not a registered MCP tool")
        for member in entry.family:
            assert member in registered, (
                f"{name}: family member {member!r} is not a registered MCP tool")


def test_lookup_unknown_tool_returns_empty_entry() -> None:
    e = lookup("totally_not_a_tool")
    assert e.family == []
    assert e.alternatives == {}


def test_has_family_true_when_alternatives_exist() -> None:
    # file.write has registered alternatives.
    assert has_family("file.write")
    # system.power.cancel is intentionally a singleton.
    assert not has_family("system.power.cancel")


# ---------------------------------------------------------------------------
# Tool registration

def test_evaluate_with_variants_registered() -> None:
    tool = find_tool("evaluate_with_variants")
    assert tool is not None
    assert tool.tier == "always"


# ---------------------------------------------------------------------------
# Handler — basic dispatch

def test_evaluate_with_variants_runs_preferred(mock_agent) -> None:
    """The preferred tool runs and its result appears in the response."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        out = tool.handler({
            "preferred": {"tool": "system.info", "arguments": {}},
        }, c)
        body = json.loads(out)
        assert body["preferred"]["tool"] == "system.info"
        assert body["preferred"]["triggered"] is True
        assert body["preferred"]["result"]  # non-empty
    finally:
        c.close()


def test_evaluate_with_variants_handles_ruled_out(mock_agent) -> None:
    """Ruled-out variants are NOT executed but ARE addressed for coverage."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        out = tool.handler({
            "preferred": {"tool": "system.info", "arguments": {}},
            "variants": [
                {"tool": "request_update_access",
                 "ruled_out_reason": "no destructive ops planned"},
            ],
        }, c)
        body = json.loads(out)
        assert len(body["variants"]) == 1
        v = body["variants"][0]
        assert v["tool"] == "request_update_access"
        assert v["ruled_out"] is True
        assert v["reason"] == "no destructive ops planned"
    finally:
        c.close()


def test_evaluate_with_variants_coverage_gap_for_known_family(mock_agent) -> None:
    """When the preferred tool is in the registry and a family member is
    not addressed, coverage_gap is populated."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        # file.write's family includes file.write_b64 and file.upload.
        # Submit only the preferred; both should appear in coverage_gap.
        out = tool.handler({
            "preferred": {
                "tool": "file.write",
                "arguments": {"path": "C:\\fake", "content": "test"},
            },
            "variants": [],
        }, c)
        body = json.loads(out)
        assert "coverage_gap" in body
        missing = {m["tool"] for m in body["coverage_gap"]["missing"]}
        assert missing == {"file.write_b64", "file.upload"}
        # Each missing entry has hint text from the registry.
        for m in body["coverage_gap"]["missing"]:
            assert m["hint"]  # non-empty
    finally:
        c.close()


def test_evaluate_with_variants_no_coverage_gap_when_addressed(mock_agent) -> None:
    """When the LLM addresses every family member (run or ruled out),
    coverage_gap is absent."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        out = tool.handler({
            "preferred": {
                "tool": "file.write",
                "arguments": {"path": "C:\\fake", "content": "test"},
            },
            "variants": [
                {"tool": "file.write_b64",
                 "ruled_out_reason": "no binary content in hand"},
                {"tool": "file.upload",
                 "ruled_out_reason": "no source file on controller"},
            ],
        }, c)
        body = json.loads(out)
        assert "coverage_gap" not in body
    finally:
        c.close()


def test_evaluate_with_variants_no_coverage_gap_for_singleton(mock_agent) -> None:
    """Tools not in the registry have no family — no coverage_gap field
    should be added regardless of variant count."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        out = tool.handler({
            "preferred": {"tool": "system.info", "arguments": {}},
        }, c)
        body = json.loads(out)
        assert "coverage_gap" not in body
    finally:
        c.close()


def test_evaluate_with_variants_rejects_malformed_preferred() -> None:
    """preferred missing 'tool' or 'arguments' raises ValueError."""
    tool = find_tool("evaluate_with_variants")
    with pytest.raises(ValueError, match="preferred must be"):
        tool.handler({"preferred": {"arguments": {}}}, None)
    with pytest.raises(ValueError, match="preferred must be"):
        tool.handler({"preferred": {"tool": "x"}}, None)


def test_evaluate_with_variants_rejects_unknown_tool() -> None:
    """Submitting a preferred tool that isn't registered raises a clear
    error before any wire interaction."""
    tool = find_tool("evaluate_with_variants")
    # Use a fake client; it shouldn't be reached.
    with pytest.raises(ValueError, match="unknown tool"):
        tool.handler({
            "preferred": {"tool": "totally_not_a_tool", "arguments": {}},
        }, _DummyClient())


def test_evaluate_with_variants_includes_feedback_hint(mock_agent) -> None:
    """Every response includes a feedback_hint encouraging reflective
    feedback, regardless of coverage status."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        tool = find_tool("evaluate_with_variants")
        out = tool.handler({
            "preferred": {"tool": "system.info", "arguments": {}},
        }, c)
        body = json.loads(out)
        assert "feedback_hint" in body
        assert "feedback" in body["feedback_hint"].lower()
    finally:
        c.close()


# ---------------------------------------------------------------------------
# Helpers

class _DummyClient:
    """Stand-in client for tests that should fail before any wire call."""
    current_tier = "read"
