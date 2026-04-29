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

"""pytest fixtures for the mcp-server smoke harness."""

from __future__ import annotations

import os
import sys
from typing import Iterator

import pytest

# Make sibling modules (agent_client.py, tools.py) and the tests/ dir
# itself (mock_agent.py lives there) importable without installing the
# package. Mirrors how server.py wires its own sys.path.
HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
sys.path.insert(0, PARENT)
sys.path.insert(0, HERE)

from mock_agent import MockAgent  # noqa: E402


@pytest.fixture
def mock_agent() -> Iterator[MockAgent]:
    """Boots a `MockAgent` on an ephemeral localhost port. Tests connect
    `AgentClient(host='127.0.0.1', port=mock_agent.port)`."""
    agent = MockAgent()
    agent.start()
    try:
        yield agent
    finally:
        agent.stop()
