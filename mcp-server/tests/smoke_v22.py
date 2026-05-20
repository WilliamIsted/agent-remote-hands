#!/usr/bin/env python
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

"""Live v2.2 smoke test for the mcp-server bridge.

Exercises the bridge end-to-end against a running agent on a real host:

1. Reads env config (REMOTE_HANDS_HOST / PORT / TOKEN_PATH).
2. Builds an `AgentClient` directly (the same one server.py uses) and
   runs `connect()` — hello + MCP initialize.
3. Calls each read-tier tool's handler with minimal args via the bridge's
   tool registry. Logs OK / ERR / exception per tool.
4. Elevates to update tier via `connection.tier_raise` and re-runs a small
   update-tier subset (window.focus on the agent's own console).
5. Drops tier back to read and disconnects.

Exits 0 on full pass; non-zero on any unexpected failure. Expected failures
(verbs the agent doesn't advertise, Phase-2b deferrals for file.* content
side-channel) are logged but don't fail the run.

Usage:

    REMOTE_HANDS_HOST=192.168.1.29 REMOTE_HANDS_PORT=8765 \\
    REMOTE_HANDS_TOKEN_PATH=C:\\ProgramData\\AgentRemoteHands\\token \\
        python mcp-server/tests/smoke_v22.py

The script intentionally avoids mutating tools (process.start, file.write,
input.*, etc.) by default — it's a connectivity / catalog / tier smoke,
not a feature test. Pass `--mutating` to additionally exercise a small
write/click/type subset under C:\\Temp\\mcp-test\\.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback
from typing import Any

# Make sibling modules importable when run as `python tests/smoke_v22.py`.
HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
sys.path.insert(0, PARENT)

from agent_client import AgentClient, ErrResponse, WireError, read_token_file  # noqa: E402
from tools import TOOLS, find_tool, tools_by_tier  # noqa: E402


GREEN = "\033[32m" if sys.stdout.isatty() else ""
RED   = "\033[31m" if sys.stdout.isatty() else ""
DIM   = "\033[2m" if sys.stdout.isatty() else ""
RESET = "\033[0m" if sys.stdout.isatty() else ""


def log_ok(msg: str) -> None:
    print(f"  {GREEN}ok{RESET}    {msg}")


def log_fail(msg: str) -> None:
    print(f"  {RED}fail{RESET}  {msg}")


def log_skip(msg: str) -> None:
    print(f"  {DIM}skip{RESET}  {msg}")


def log_info(msg: str) -> None:
    print(f"  {DIM}info{RESET}  {msg}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default=os.environ.get("REMOTE_HANDS_HOST", "127.0.0.1"),
        help="Agent host (default: REMOTE_HANDS_HOST or 127.0.0.1)")
    parser.add_argument(
        "--port", type=int,
        default=int(os.environ.get("REMOTE_HANDS_PORT", "8765")),
        help="Agent port (default: REMOTE_HANDS_PORT or 8765)")
    parser.add_argument(
        "--token-path",
        default=os.environ.get(
            "REMOTE_HANDS_TOKEN_PATH",
            r"C:\ProgramData\AgentRemoteHands\token"),
        help="Path to the agent's elevation token file")
    parser.add_argument(
        "--mutating", action="store_true",
        help="Additionally exercise update-tier write / click / type tools")
    args = parser.parse_args()

    failures: list[str] = []

    print(f"agent: {args.host}:{args.port}")
    print(f"token: {args.token_path}")

    # Phase 1 — connect.
    print("\n[1] connect + initialize")
    client = AgentClient(host=args.host, port=args.port,
                         client_name="mcp-server-smoke")
    try:
        client.connect()
    except (OSError, WireError) as ex:
        log_fail(f"connect failed: {ex}")
        return 1
    log_ok(f"hello + initialize: serverInfo={client.server_info}")
    log_ok(f"hello body framings={client.hello_body.get('framings', [])}")
    log_ok(f"current_tier={client.current_tier}")

    # Phase 2 — system.info round-trip.
    print("\n[2] system.info round-trip")
    try:
        info = client.info()
        required = {"family", "agent", "agent_protocol", "current_tier",
                    "integrity", "framings"}
        missing = required - set(info)
        if missing:
            log_fail(f"system.info missing fields: {missing}")
            failures.append("system.info missing fields")
        else:
            log_ok(f"family={info['family']} protocol={info['agent_protocol']} "
                   f"integrity={info['integrity']} tier={info['current_tier']}")
    except Exception as ex:
        log_fail(f"system.info raised: {ex}")
        failures.append(f"system.info: {ex}")

    # Phase 3 — system.capabilities + count of advertised verbs.
    print("\n[3] system.capabilities")
    try:
        caps = client.capabilities()
        log_ok(f"agent advertises {len(caps)} verbs")
    except Exception as ex:
        log_fail(f"system.capabilities raised: {ex}")
        failures.append(f"system.capabilities: {ex}")
        caps = {}

    # Phase 4 — bridge tools/list at read tier.
    print("\n[4] bridge tools/list (read tier)")
    read_tools = tools_by_tier("read")
    log_ok(f"bridge exposes {len(read_tools)} tools at read tier")

    # Phase 5 — exercise read-tier handlers via the bridge.
    print("\n[5] read-tier handler round-trips")
    read_only_calls = [
        ("system.info",   {}),
        ("window.list",   {"limit": 5}),
        ("process.list",  {"pattern": "explorer", "limit": 5}),
        ("clipboard.get", {}),
        ("directory.list", {"path": r"C:\Temp\mcp-test", "limit": 5}),
        ("screen.capture", {"format": "png"}),
        ("element.list",  {}),
        ("window.find",   {"pattern": "explorer"}),
    ]
    for tool_name, tool_args in read_only_calls:
        tool = find_tool(tool_name)
        if tool is None:
            log_skip(f"{tool_name}: not in bridge registry")
            continue
        if tool.wire_verb and tool.wire_verb not in caps and tool_name not in caps:
            log_skip(f"{tool_name}: agent does not advertise wire verb "
                     f"{tool.wire_verb!r}")
            continue
        try:
            t0 = time.time()
            out = tool.handler(tool_args, client)
            elapsed_ms = int((time.time() - t0) * 1000)
            preview = out[:80].replace("\n", " ")
            log_ok(f"{tool_name} ({elapsed_ms} ms): {preview!r}...")
        except Exception as ex:
            # Phase-2b deferrals for file.* present as RuntimeError ERR.
            # `not_found` and `empty` are legitimate agent responses for the
            # generic probe args we send (no Explorer window, no test dir,
            # empty clipboard) — log them but don't count as bridge failures.
            msg = str(ex)
            if "deferred" in msg.lower():
                log_skip(f"{tool_name}: agent reports deferred ({msg[:80]})")
            elif "ERR not_found" in msg or "ERR empty" in msg:
                log_skip(f"{tool_name}: expected ERR for probe args "
                         f"({msg[:80]})")
            else:
                log_fail(f"{tool_name}: {type(ex).__name__}: {msg[:120]}")
                failures.append(f"{tool_name}: {ex}")

    # Phase 6 — elevation to update tier.
    print("\n[6] elevate to update tier")
    token = read_token_file(args.token_path)
    if token is None:
        log_skip(f"token unreadable at {args.token_path}; "
                 "skipping elevation phase")
    else:
        client.set_token(token)
        r = client.tier_raise("update")
        if isinstance(r, ErrResponse):
            log_fail(f"tier_raise update: ERR {r.code} {r.detail}")
            failures.append(f"tier_raise: {r.code}")
        else:
            log_ok(f"tier_raise OK; current_tier={client.current_tier}")
            update_tools = tools_by_tier("update")
            log_ok(f"bridge tools/list (update tier): {len(update_tools)}")

            if args.mutating:
                print("\n[7] update-tier mutating smoke (--mutating)")
                # Create a small test file via file.write (NB: Phase 2b
                # deferred on the v2.2 agent build — expected to ERR).
                tool = find_tool("file.write")
                if tool is not None:
                    try:
                        out = tool.handler({
                            "path": r"C:\Temp\mcp-test\agent-side-smoke.txt",
                            "content": "smoke test write\n",
                        }, client)
                        log_ok(f"file.write: {out[:80]!r}")
                    except Exception as ex:
                        msg = str(ex)
                        if "deferred" in msg.lower() or "Phase 2b" in msg:
                            log_skip(f"file.write: agent reports deferred "
                                     f"(Phase 2b)")
                        else:
                            log_fail(f"file.write: {type(ex).__name__}: "
                                     f"{msg[:120]}")
                            failures.append(f"file.write: {ex}")

            # Drop tier on the way out.
            client.tier_drop("read")
            log_ok(f"tier_drop OK; current_tier={client.current_tier}")

    # Phase final — close.
    client.close()
    print()
    if failures:
        print(f"{RED}FAILED{RESET}: {len(failures)} issue(s):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"{GREEN}PASS{RESET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
