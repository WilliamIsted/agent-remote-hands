#!/usr/bin/env python3
"""Conformance test runner for Agent Remote Hands.

Discovers every test_*.py file in this directory and runs them against a live
agent. Each test that needs a verb the agent doesn't advertise is SKIPPED;
verbs the agent claims to implement are tested strictly. Same suite runs
against every target — that's how we keep the feature-set comparable across
windows-nt, windows-9x, windows-modern, etc.

Usage:
    python run.py <host> [port]
    REMOTE_HANDS_HOST=192.168.x.x python run.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from fixtures import ConformanceTestCase  # noqa: E402


def main():
    host = None
    port = 8765
    timeout = 30
    args = sys.argv[1:]
    if args:
        host = args[0]
        if len(args) >= 2:
            port = int(args[1])
    if not host:
        host = os.environ.get("REMOTE_HANDS_HOST")
    if not host:
        env_port = os.environ.get("REMOTE_HANDS_PORT")
        if env_port:
            port = int(env_port)
    if not host:
        print("usage: run.py <host> [port]   (or set REMOTE_HANDS_HOST)",
              file=sys.stderr)
        return 2

    ConformanceTestCase.host = host
    ConformanceTestCase.port = port
    ConformanceTestCase.timeout = timeout

    print(f"--- conformance against {host}:{port} ---")
    # Probe early so we fail fast if the agent isn't reachable
    try:
        caps = ConformanceTestCase._query_caps()
        info = ConformanceTestCase._query_info()
    except Exception as e:
        print(f"cannot reach agent at {host}:{port}: {e}", file=sys.stderr)
        return 2
    print(f"target: os={info.get('os', '?')} "
          f"arch={info.get('arch', '?')} "
          f"protocol={info.get('protocol', '?')}")
    print(f"verbs:  {len(caps)} advertised in CAPS")
    print()

    loader = unittest.TestLoader()
    suite = loader.discover(HERE, pattern="test_*.py")
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
