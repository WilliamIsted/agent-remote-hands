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

"""Tests for `system.*` namespace verbs."""

import json

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


REQUIRED_INFO_FIELDS = {
    "name", "version", "protocol", "os", "arch", "hostname",
    "tiers", "current_tier", "auth", "max_connections",
    "namespaces", "capabilities",
}


def test_info_has_required_fields(client: WireClient) -> None:
    info = client.info()
    missing = REQUIRED_INFO_FIELDS - set(info)
    assert not missing, f"system.info missing fields: {missing}"


def test_info_protocol_is_v2(client: WireClient) -> None:
    info = client.info()
    assert info["protocol"].startswith("2"), f"got {info['protocol']!r}"


def test_info_advertises_five_tiers(client: WireClient) -> None:
    info = client.info()
    assert "read"        in info["tiers"]
    assert "create"      in info["tiers"]
    assert "update"      in info["tiers"]
    assert "delete"      in info["tiers"]
    assert "extra_risky" in info["tiers"]


def test_info_namespaces_includes_connection(client: WireClient) -> None:
    info = client.info()
    assert "connection" in info["namespaces"]


def test_capabilities_advertises_system_info(client: WireClient) -> None:
    caps = client.capabilities()
    assert caps.get("system.info", {}).get("tier") == "read"


# ---------------------------------------------------------------------------
# system.verbs — full strict-tool spec corpus over the wire

def test_verbs_returns_verbs_object(client: WireClient,
                                    capabilities: dict) -> None:
    """system.verbs returns `{verbs: {<name>: <strict-tool-def>, ...}}`."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    assert "verbs" in body
    assert isinstance(body["verbs"], dict)
    assert body["verbs"], "verbs map must be non-empty"


def test_verbs_entries_are_strict_tool_defs(client: WireClient,
                                             capabilities: dict) -> None:
    """Each map value has the required strict-tool keys."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    body = json.loads(r.payload)
    for verb_name, defn in body["verbs"].items():
        assert isinstance(defn, dict), f"{verb_name}: value is not an object"
        assert defn.get("name") == verb_name, \
            f"{verb_name}: `name` field must match the map key"
        assert isinstance(defn.get("description"), str) and defn["description"], \
            f"{verb_name}: missing/empty description"
        schema = defn.get("input_schema")
        assert isinstance(schema, dict) and schema.get("type") == "object", \
            f"{verb_name}: input_schema must be an object schema"


def test_verbs_superset_of_capabilities(client: WireClient,
                                        capabilities: dict) -> None:
    """Every verb in system.capabilities must also appear in system.verbs."""
    needs_verb(capabilities, "system.verbs")
    r = client.request("system.verbs")
    body = json.loads(r.payload)
    advertised = set(capabilities.keys())
    served = set(body["verbs"].keys())
    missing = advertised - served
    assert not missing, \
        f"system.capabilities verbs absent from system.verbs: {sorted(missing)}"


# ---------------------------------------------------------------------------

def test_health_succeeds(client: WireClient) -> None:
    r = client.request("system.health")
    assert isinstance(r, OkResponse)


def test_reboot_requires_extra_risky_tier(client: WireClient,
                                          capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.reboot")
    r = client.request("system.power.reboot")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "extra_risky"


def test_shutdown_blockers_returns_array(client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.blockers")
    r = client.request("system.power.blockers")
    assert isinstance(r, OkResponse)
    import json
    body = json.loads(r.payload)
    assert "blockers" in body
    assert isinstance(body["blockers"], list)


def test_power_cancel_requires_update_tier(client: WireClient,
                                           capabilities: dict) -> None:
    """system.power.cancel is x-crudx=U → update tier per spec §4.1."""
    needs_verb(capabilities, "system.power.cancel")
    r = client.request("system.power.cancel")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "update"


def test_power_cancel_no_pending_returns_not_found(extra_risky_client: WireClient,
                                                   capabilities: dict) -> None:
    needs_verb(capabilities, "system.power.cancel")
    r = extra_risky_client.request("system.power.cancel")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_found"


def test_power_delay_overlap_conflicts_then_cancels(
        extra_risky_client: WireClient, capabilities: dict) -> None:
    """Schedule a long-delay shutdown, observe pending state via overlap,
    then cancel. Uses --delay 86400 so the machine remains safe even if
    cancellation regresses (24h grace to intervene manually)."""
    needs_verb(capabilities, "system.power.cancel")
    needs_verb(capabilities, "system.power.shutdown")

    # Schedule a 24-hour delayed shutdown.
    r = extra_risky_client.request("system.power.shutdown", "--delay", "86400")
    assert isinstance(r, OkResponse), f"got {r!r}"
    try:
        # A second --delay request must be rejected.
        r2 = extra_risky_client.request("system.power.shutdown", "--delay", "86400")
        assert isinstance(r2, ErrResponse)
        assert r2.code == "conflict"
        assert "pending_until_ms" in r2.detail
        assert isinstance(r2.detail["pending_until_ms"], int)
    finally:
        # Always cancel — leaving a pending OS-level shutdown around between
        # tests is unfriendly even with a 24h delay.
        r3 = extra_risky_client.request("system.power.cancel")
        assert isinstance(r3, OkResponse), f"cancel failed: {r3!r}"

    # And a second cancel returns not_found.
    r4 = extra_risky_client.request("system.power.cancel")
    assert isinstance(r4, ErrResponse)
    assert r4.code == "not_found"
