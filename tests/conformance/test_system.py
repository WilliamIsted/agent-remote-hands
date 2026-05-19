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

"""Tests for `system.*` and `system.power.*` namespaces.

Verb-name note: the v1 `system.shutdown` / `system.reboot` / `system.logoff` /
`system.lock` / `system.hibernate` / `system.sleep` / `system.shutdown_blockers`
were re-namespaced into `system.power.*` in v2.1.0-rc.2. Tests below exclusively
exercise the v2.1+ names."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# Fields the post-rc.2 system.info schema requires (per spec/verbs/system.info.json).
REQUIRED_INFO_FIELDS = {
    "family", "agent", "agent_protocol", "os_name", "os_version",
    "cpu_arch", "integrity", "uiaccess", "hostname", "screens",
    "capabilities", "current_tier", "framings",
}


# ---------------------------------------------------------------------------
# system.info — verb-discovery / capability advertisement

def test_info_has_required_fields(client: WireClient) -> None:
    info = client.info()
    missing = REQUIRED_INFO_FIELDS - set(info)
    assert not missing, f"system.info missing fields: {missing}"


def test_info_family_is_known(client: WireClient) -> None:
    """`family` is the strict-enum source for routing per-OS verb behaviour."""
    info = client.info()
    assert info["family"] in {"windows-modern", "windows-classic", "windows-legacy"}, \
        f"unknown family: {info['family']!r}"


def test_info_agent_protocol_is_v2(client: WireClient) -> None:
    info = client.info()
    assert info["agent_protocol"].startswith("2"), \
        f"got {info['agent_protocol']!r}"


def test_info_screens_is_non_empty_array(client: WireClient) -> None:
    """Every system with a display reports at least one entry; per-monitor
    bounds are nested per the post-#74 strict-typing."""
    info = client.info()
    assert isinstance(info["screens"], list)
    assert len(info["screens"]) >= 1
    for s in info["screens"]:
        assert isinstance(s["index"], int)
        assert s["index"] >= 0
        for k in ("x", "y", "w", "h"):
            assert k in s["bounds"], f"screen entry missing bounds.{k}: {s}"
        assert isinstance(s["primary"], bool)


def test_info_cpu_arch_in_known_set(client: WireClient) -> None:
    info = client.info()
    assert info["cpu_arch"] in {"x86", "x64", "arm64", "arm"}


def test_info_integrity_in_known_set(client: WireClient) -> None:
    info = client.info()
    assert info["integrity"] in {"low", "medium", "high", "system", "none"}


def test_info_capabilities_is_object(client: WireClient) -> None:
    """`capabilities` is the open-ended sub-cap map (strict:false carve-out)."""
    info = client.info()
    assert isinstance(info["capabilities"], dict)


def test_info_current_tier_is_read_after_hello(client: WireClient) -> None:
    """A freshly-hello'd connection defaults to read tier; system.info.current_tier
    echoes that without the caller having to track tier client-side."""
    info = client.info()
    assert info["current_tier"] == "read"


# ---------------------------------------------------------------------------
# system.capabilities — verb→tier discovery map

def test_capabilities_advertises_system_info(client: WireClient) -> None:
    caps = client.capabilities()
    assert caps.get("system.info", {}).get("tier") == "read"


def test_capabilities_tier_values_are_known(client: WireClient) -> None:
    """Every advertised verb's tier must be one of the five canonical tiers."""
    caps = client.capabilities()
    valid = {"read", "create", "update", "delete", "extra_risky"}
    for verb, descriptor in caps.items():
        tier = descriptor.get("tier")
        assert tier in valid, f"{verb} has unknown tier {tier!r}"


# ---------------------------------------------------------------------------
# system.verbs — full strict-tool defs over the wire (issue #97; since v2.2)

def test_verbs_returns_verbs_object(
        client: WireClient, capabilities: dict) -> None:
    """system.verbs returns `{verbs: {<name>: <strict-tool-def>, ...}}`."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "verbs" in body
    assert isinstance(body["verbs"], dict)
    assert body["verbs"], "verbs map must be non-empty"


def test_verbs_entries_are_strict_tool_defs(
        client: WireClient, capabilities: dict) -> None:
    """Each map value is a strict-tool definition with the required keys
    (name, description, input_schema). x-* extensions are allowed but not
    required by this assertion — the agent is free to strip or preserve."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    body = json.loads(r.payload)
    for verb_name, definition in body["verbs"].items():
        assert isinstance(definition, dict), f"{verb_name}: not an object"
        assert definition.get("name") == verb_name, \
            f"{verb_name}: `name` field must match the map key"
        assert isinstance(definition.get("description"), str) and definition["description"], \
            f"{verb_name}: missing/empty description"
        schema = definition.get("input_schema")
        assert isinstance(schema, dict) and schema.get("type") == "object", \
            f"{verb_name}: input_schema must be an object schema"


def test_verbs_superset_of_capabilities(
        client: WireClient, capabilities: dict) -> None:
    """Every verb advertised by `system.capabilities` must also appear in
    `system.verbs`. The reverse is not required (system.verbs MAY include
    verbs the agent registers but doesn't gate via capabilities — though
    today every dispatcher entry shows up in both)."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    body = json.loads(r.payload)
    advertised = set(capabilities.keys())
    served = set(body["verbs"].keys())
    missing = advertised - served
    assert not missing, f"system.capabilities advertises verbs not in system.verbs: {sorted(missing)}"


# ---------------------------------------------------------------------------
# system.health

def test_health_succeeds(client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.health")
    r = client.request("system.health")
    assert isinstance(r, OkResponse)


# ---------------------------------------------------------------------------
# system.power.* — extra-risky verbs (tier gate is the safe assertion)

def test_power_shutdown_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.shutdown")
    r = client.request("system.power.shutdown")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_reboot_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.reboot")
    r = client.request("system.power.reboot")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_logoff_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.logoff")
    r = client.request("system.power.logoff")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_hibernate_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.hibernate")
    r = client.request("system.power.hibernate")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_sleep_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.sleep")
    r = client.request("system.power.sleep")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_lock_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.lock")
    r = client.request("system.power.lock")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_blockers_returns_array(
        client: WireClient, capabilities: dict) -> None:
    """Read-tier — call should succeed without elevation."""
    needs_verb(capabilities, "system.power.blockers")
    r = client.request("system.power.blockers")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert "blockers" in body
    assert isinstance(body["blockers"], list)
    for b in body["blockers"]:
        assert "handle" in b and "reason" in b
        assert b["handle"].startswith("win:")


def test_power_cancel_requires_extra_risky_tier(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.cancel")
    r = client.request("system.power.cancel")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_power_cancel_no_pending_returns_not_found(
        extra_risky_client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.cancel")
    r = extra_risky_client.request("system.power.cancel")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_power_delay_overlap_conflicts_then_cancels(
        extra_risky_client: WireClient, capabilities: dict) -> None:
    """Schedule a long-delay shutdown via system.power.shutdown,
    observe pending state via overlap, then cancel via system.power.cancel.
    Uses --delay-seconds 86400 so the machine remains safe even if cancellation
    regresses (24h grace to intervene manually)."""
    needs_verb(capabilities, "system.power.cancel")
    needs_verb(capabilities, "system.power.shutdown")

    r = extra_risky_client.request("system.power.shutdown",
                                   "--delay-seconds", "86400")
    assert isinstance(r, OkResponse), f"got {r!r}"
    try:
        r2 = extra_risky_client.request("system.power.shutdown",
                                        "--delay-seconds", "86400")
        assert isinstance(r2, ErrResponse)
        assert r2.code == "conflict"
    finally:
        # Always cancel — leaving a pending OS-level shutdown around between
        # tests is unfriendly even with a 24h delay.
        r3 = extra_risky_client.request("system.power.cancel")
        assert isinstance(r3, OkResponse), f"cancel failed: {r3!r}"

    # And a second cancel returns not_found.
    r4 = extra_risky_client.request("system.power.cancel")
    assert isinstance(r4, ErrResponse)
    assert r4.code == "not_found"
