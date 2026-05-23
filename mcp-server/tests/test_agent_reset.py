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

"""Tests for the bridge-side recovery primitive (``AgentClient.reset()``),
the manual ``agent_reset`` MCP tool, and the auto-recovery wrapper in
``server.py``.

Background: the v2.2 spec does not expose a wire-level reset (§1.6.7), and
even if it did, the failure mode that wedged Session 2 lives on the bridge
side — the agent's parser stays healthy. Recovery has to mean
"reconnect the TCP socket from the bridge", which is the primitive
``AgentClient.reset()`` provides.
"""

from __future__ import annotations

import asyncio
import socket
from typing import Any

import pytest

from agent_client import AgentClient, OkResponse, WireError


# ======================================================================
# (1) AgentClient.reset() — direct unit tests
# ======================================================================

def test_reset_recovers_from_poisoned_buf(mock_agent) -> None:
    """The Session-2 scenario: ``_buf`` has stale bytes that don't form a
    valid Content-Length header. The next call raises WireError; reset()
    restores normal operation."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        # Sanity: the bridge works in its clean state.
        info_before = c.info()
        assert info_before["protocol"] == "2.1"

        # Poison the buffer with garbage that doesn't form a valid MCP
        # header line. The next read will try to parse this and fail.
        c._buf.extend(b"garbage-no-colon-here-\xff\xff\r\n\r\n")

        # The bridge raises WireError on the next forwarded call. The
        # `connect()` call wrapped client.info() once already, but the next
        # one trips over the poisoned buf.
        with pytest.raises(WireError):
            c.info()

        # Reset recovers. Tier resets to "read" by design (security default).
        result = c.reset()
        assert result["ok"] is True, result
        assert result["new_tier"] == "read"
        assert result["forced_socket_close"] is False

        # Bridge is usable again.
        info_after = c.info()
        assert info_after["protocol"] == "2.1"
    finally:
        c.close()


def test_reset_drops_to_read_even_if_prior_tier_was_elevated(mock_agent) -> None:
    """After a successful elevation, reset must drop tier back to read.
    The cached token survives (set_token state on the client is not wiped),
    so the LLM can re-elevate by calling request_*_access again."""
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        r = c.tier_raise("update")
        assert isinstance(r, OkResponse)
        assert c.current_tier == "update"

        result = c.reset()
        assert result["ok"] is True
        assert result["prior_tier"] == "update"
        assert result["new_tier"] == "read"
        assert c.current_tier == "read"

        # Re-elevation works — the token is still set on the client.
        r2 = c.tier_raise("update")
        assert isinstance(r2, OkResponse)
        assert c.current_tier == "update"
    finally:
        c.close()


def test_reset_reports_reconnect_failed_when_agent_unreachable(mock_agent) -> None:
    """If the agent is unreachable when reset tries to reconnect, the
    result is ``{ok: False, error: "reconnect_failed", ...}`` — not an
    exception. The bridge stays alive so the LLM can be told what went
    wrong instead of seeing the MCP server crash."""
    c = AgentClient("127.0.0.1", mock_agent.port, timeout=1.0)
    c.connect()
    try:
        # Take the mock down: future connections will be refused.
        mock_agent.stop()
        result = c.reset()
        assert result["ok"] is False
        assert result["error"] == "reconnect_failed"
        assert "detail" in result
        # New tier reported as "read" because that's what it would be after
        # a successful reset — the bridge isn't lying about a partial state.
        assert result["new_tier"] == "read"
    finally:
        try:
            c.close()
        except Exception:
            pass


def test_reset_force_closes_socket_when_lock_held(mock_agent) -> None:
    """If another thread is holding the lock (e.g. wedged in recv on a
    half-dead socket), reset force-closes the socket from outside the lock
    to unblock the wedged caller, then proceeds with the clean reconnect.

    Simulated here by acquiring the lock from a helper thread and holding
    it past the reset's lock_timeout. The forced close is observable via
    the result's ``forced_socket_close`` field."""
    import threading

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        lock_released = threading.Event()
        lock_acquired = threading.Event()

        def hold_the_lock():
            with c._lock:
                lock_acquired.set()
                # Hold the lock briefly — longer than reset's 2.0s timeout
                # so the force-close path fires, but not so long the test
                # times out itself.
                lock_released.wait(timeout=5.0)

        holder = threading.Thread(target=hold_the_lock, daemon=True)
        holder.start()
        lock_acquired.wait(timeout=2.0)

        # Reset with a short lock_timeout. The holder thread blocks the
        # lock; reset force-closes the socket. The holder is sleeping on
        # the event, not on recv, so the force-close doesn't unblock IT —
        # but as soon as the holder releases (we set the event), reset's
        # second acquire succeeds.
        def trigger_release_after(delay: float):
            import time
            time.sleep(delay)
            lock_released.set()

        releaser = threading.Thread(
            target=trigger_release_after, args=(0.5,), daemon=True)
        releaser.start()

        result = c.reset(lock_timeout=0.1)
        assert result["forced_socket_close"] is True
        assert result["ok"] is True

        holder.join(timeout=2.0)
    finally:
        c.close()


# ======================================================================
# (2) Manual `agent_reset` MCP tool
# ======================================================================

def test_agent_reset_tool_requires_reason(mock_agent) -> None:
    from tools import _h_agent_reset

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        with pytest.raises(ValueError, match="reason is required"):
            _h_agent_reset({}, c)
        with pytest.raises(ValueError, match="reason is required"):
            _h_agent_reset({"reason": "   "}, c)
    finally:
        c.close()


def test_agent_reset_tool_returns_structured_json(mock_agent) -> None:
    import json
    from tools import _h_agent_reset

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        text = _h_agent_reset(
            {"reason": "test: probe the manual reset path"}, c)
        parsed = json.loads(text)
        assert parsed["ok"] is True
        assert parsed["reason"] == "test: probe the manual reset path"
        assert parsed["new_tier"] == "read"
    finally:
        c.close()


def test_agent_reset_tool_is_registered_and_always_tier() -> None:
    from tools import TOOLS, find_tool
    tool = find_tool("agent_reset")
    assert tool is not None, "agent_reset must be registered"
    assert tool.tier == "always", (
        "agent_reset must be always-tier so it's callable when the "
        "bridge is wedged regardless of prior tier state")


# ======================================================================
# (3) Auto-recovery wrapper (server.py::_call_with_recovery)
# ======================================================================

class _StubTool:
    """Minimal ToolDef-shaped object — just the attributes the wrapper
    reads. Lets us drive the wrapper without instantiating real handlers."""
    def __init__(self, *, name: str, read_only_hint: bool, handler):
        self.name = name
        self.read_only_hint = read_only_hint
        self.handler = handler


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_auto_recovery_retries_read_only_verb_after_wire_error(mock_agent) -> None:
    """A read_only_hint=True verb that raises WireError once is auto-recovered:
    the bridge resets and retries; the retry's success is returned to the
    LLM with a clear recovery note."""
    from server import _call_with_recovery

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        call_count = {"n": 0}

        def handler(args, client):
            call_count["n"] += 1
            if call_count["n"] == 1:
                raise WireError("simulated wire desync (test)")
            return "RETRY_OK"

        tool = _StubTool(
            name="test.read_only_thing",
            read_only_hint=True,
            handler=handler)

        result = _run(_call_with_recovery(c, tool, {}))
        assert call_count["n"] == 2, "handler must be retried exactly once"
        assert "RETRY_OK" in result
        assert "auto-reset succeeded" in result, result
    finally:
        c.close()


def test_auto_recovery_skips_retry_for_non_read_only_verb(mock_agent) -> None:
    """A non-read-only verb that raises WireError gets a structured
    wire_error message — no auto-retry. The verb may have had side effects
    that aren't safe to repeat."""
    from server import _call_with_recovery

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        call_count = {"n": 0}

        def handler(args, client):
            call_count["n"] += 1
            raise WireError("simulated wire desync (test)")

        tool = _StubTool(
            name="test.destructive_thing",
            read_only_hint=False,
            handler=handler)

        result = _run(_call_with_recovery(c, tool, {}))
        assert call_count["n"] == 1, (
            "non-read-only verbs must not be auto-retried — side-effect "
            "safety can't be guaranteed")
        assert "wire_error" in result
        assert "agent_reset" in result, (
            "non-read-only WireError surface must direct the LLM to the "
            "manual escape hatch")
    finally:
        c.close()


def test_auto_recovery_reports_reset_failure(mock_agent) -> None:
    """If the auto-recovery reset itself fails (e.g. agent gone away),
    the LLM gets a clear message naming the reset failure — not a Python
    traceback. Closes #78 (WireError traceback leak) as a side-effect of
    the structured wire_error surface."""
    from server import _call_with_recovery

    c = AgentClient("127.0.0.1", mock_agent.port, timeout=1.0)
    c.connect()

    def handler(args, client):
        raise WireError("simulated wire desync (test)")

    tool = _StubTool(
        name="test.read_only_unreachable",
        read_only_hint=True,
        handler=handler)

    try:
        mock_agent.stop()  # take the agent down before retry
        result = _run(_call_with_recovery(c, tool, {}))
        assert "wire_error" in result
        assert "auto-recovery and failed" in result, result
        assert "reconnect_failed" in result
        # NOT a Python traceback in the LLM-facing surface.
        assert "Traceback" not in result
    finally:
        try:
            c.close()
        except Exception:
            pass
