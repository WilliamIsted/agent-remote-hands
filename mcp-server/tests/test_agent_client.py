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

"""End-to-end tests for `agent_client.AgentClient` against the in-process
mock agent (`tests/mock_agent.py`). Exercises the wire framing, hello
handshake, tier transitions, and error surface — without needing a real
Windows agent."""

from __future__ import annotations

from agent_client import AgentClient, ErrResponse, OkResponse


def test_connect_succeeds_and_starts_at_read(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        assert c.current_tier == "read"
    finally:
        c.close()


def test_info_returns_stub_json(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        info = c.info()
        assert info["protocol"] == "2.1"
        assert info["current_tier"] == "read"
        assert "update" in info["tiers"]
        assert "extra_risky" in info["tiers"]
    finally:
        c.close()


def test_tier_raise_with_bad_token_returns_auth_invalid(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token("wrong-token")
    try:
        r = c.tier_raise("update")
        assert isinstance(r, ErrResponse)
        assert r.code == "auth_invalid"
        assert c.current_tier == "read"
    finally:
        c.close()


def test_tier_raise_with_correct_token_advances(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        r = c.tier_raise("update")
        assert isinstance(r, OkResponse)
        assert c.current_tier == "update"
        # extra_risky requires a further raise from update via the ladder.
        r2 = c.tier_raise("extra_risky")
        assert isinstance(r2, OkResponse)
        assert c.current_tier == "extra_risky"
    finally:
        c.close()


def test_tier_drop_returns_to_read(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    c.set_token(mock_agent.token)
    try:
        c.tier_raise("update")
        assert c.current_tier == "update"
        r = c.tier_drop("read")
        assert isinstance(r, OkResponse)
        assert c.current_tier == "read"
    finally:
        c.close()


def test_can_satisfy_respects_ladder_order() -> None:
    c = AgentClient("127.0.0.1", 0)  # not connected — tier check is local
    assert c.current_tier == "read"
    assert c.can_satisfy("read")
    assert not c.can_satisfy("create")
    assert not c.can_satisfy("update")
    assert not c.can_satisfy("delete")
    assert not c.can_satisfy("extra_risky")
    c._current_tier = "update"
    assert c.can_satisfy("read")
    assert c.can_satisfy("create")
    assert c.can_satisfy("update")
    assert not c.can_satisfy("delete")
    assert not c.can_satisfy("extra_risky")
    c._current_tier = "extra_risky"
    assert c.can_satisfy("read")
    assert c.can_satisfy("create")
    assert c.can_satisfy("update")
    assert c.can_satisfy("delete")
    assert c.can_satisfy("extra_risky")


def test_unknown_verb_surfaces_err_not_supported(mock_agent) -> None:
    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        r = c.request("nonsense.verb")
        assert isinstance(r, ErrResponse)
        assert r.code == "not_supported"
        assert r.detail.get("verb") == "nonsense.verb"
    finally:
        c.close()


def test_custom_handler_registers_a_verb(mock_agent) -> None:
    """Tests can plug verb handlers in for whatever shape they want to
    exercise — used by tools tests for individual wire verbs."""
    def handler(args, payload):
        return ("ok", {"echo": " ".join(args)})

    mock_agent.register("test.echo", handler)

    c = AgentClient("127.0.0.1", mock_agent.port)
    c.connect()
    try:
        r = c.request("test.echo", "hello", "world")
        assert isinstance(r, OkResponse)
        assert r.json()["echo"] == "hello world"
    finally:
        c.close()
