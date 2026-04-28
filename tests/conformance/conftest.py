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

"""pytest fixtures for the conformance suite."""

from __future__ import annotations

import os
import pathlib
from typing import Iterator

import pytest

from wire import ErrResponse, WireClient


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--host",
        default=os.environ.get("REMOTE_HANDS_HOST", "127.0.0.1"),
        help="Agent host (default: REMOTE_HANDS_HOST or 127.0.0.1).",
    )
    parser.addoption(
        "--port",
        type=int,
        default=int(os.environ.get("REMOTE_HANDS_PORT", "8765")),
        help="Agent port (default: REMOTE_HANDS_PORT or 8765).",
    )
    parser.addoption(
        "--token-path",
        default=os.environ.get(
            "REMOTE_HANDS_TOKEN_PATH",
            r"C:\ProgramData\AgentRemoteHands\token",
        ),
        help="Path to the agent's elevation token file.",
    )


@pytest.fixture(scope="session")
def host(pytestconfig: pytest.Config) -> str:
    return pytestconfig.getoption("host")


@pytest.fixture(scope="session")
def port(pytestconfig: pytest.Config) -> int:
    return pytestconfig.getoption("port")


@pytest.fixture(scope="session")
def token(pytestconfig: pytest.Config) -> str:
    p = pathlib.Path(pytestconfig.getoption("token_path"))
    if not p.exists():
        pytest.skip(f"token file not readable at {p}")
    return p.read_text(encoding="ascii").strip()


@pytest.fixture(scope="session")
def capabilities(host: str, port: int) -> dict:
    """Map of verb name -> {'tier': '<observe|drive|power>'}."""
    with WireClient(host, port) as c:
        c.hello()
        return c.capabilities()


@pytest.fixture
def client(host: str, port: int) -> Iterator[WireClient]:
    """Per-test connection at observe tier."""
    with WireClient(host, port) as c:
        c.hello()
        yield c


@pytest.fixture
def drive_client(client: WireClient, token: str) -> WireClient:
    """Per-test connection elevated to drive tier."""
    r = client.tier_raise("drive", token)
    if isinstance(r, ErrResponse):
        pytest.skip(f"could not elevate to drive: {r.code} {r.detail}")
    return client


@pytest.fixture
def power_client(client: WireClient, token: str) -> WireClient:
    """Per-test connection elevated to power tier."""
    r = client.tier_raise("drive", token)
    if isinstance(r, ErrResponse):
        pytest.skip(f"could not elevate to drive: {r.code}")
    r = client.tier_raise("power", token)
    if isinstance(r, ErrResponse):
        pytest.skip(f"could not elevate to power: {r.code}")
    return client


def needs_verb(capabilities: dict, verb: str) -> None:
    """Helper for tests: skip if the agent does not advertise the verb."""
    if verb not in capabilities:
        pytest.skip(f"agent does not advertise {verb}")
