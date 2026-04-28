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


def test_info_advertises_three_tiers(client: WireClient) -> None:
    info = client.info()
    assert "observe" in info["tiers"]
    assert "drive"   in info["tiers"]
    assert "power"   in info["tiers"]


def test_info_namespaces_includes_connection(client: WireClient) -> None:
    info = client.info()
    assert "connection" in info["namespaces"]


def test_capabilities_advertises_system_info(client: WireClient) -> None:
    caps = client.capabilities()
    assert caps.get("system.info", {}).get("tier") == "observe"


def test_health_succeeds(client: WireClient) -> None:
    r = client.request("system.health")
    assert isinstance(r, OkResponse)


def test_reboot_requires_power_tier(client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "system.reboot")
    r = client.request("system.reboot")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"
    assert r.detail.get("required") == "power"


def test_shutdown_blockers_returns_array(client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "system.shutdown_blockers")
    r = client.request("system.shutdown_blockers")
    assert isinstance(r, OkResponse)
    import json
    body = json.loads(r.payload)
    assert "blockers" in body
    assert isinstance(body["blockers"], list)
