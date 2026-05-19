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

"""Convenience entry point for the conformance suite.

Usage:
    python tests/conformance/run.py <host> [port]

This is a thin wrapper around `pytest`; advanced users can invoke pytest
directly:
    pytest tests/conformance --host 192.168.1.42 --port 8765
"""

from __future__ import annotations

import pathlib
import sys

import pytest


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    host = argv[1]
    port = argv[2] if len(argv) > 2 else "8765"

    here = pathlib.Path(__file__).resolve().parent
    args = [
        str(here),
        "--host", host,
        "--port", port,
        "-v",
    ]
    return pytest.main(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
