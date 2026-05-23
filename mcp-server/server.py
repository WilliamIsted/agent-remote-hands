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

"""MCP server bridge for Agent Remote Hands.

Exposes the wire-protocol verbs as named MCP tools to LLM clients (Claude
Code, Claude Desktop, etc.). The exposed tool surface is filtered by the
current connection tier on the v2.1 CRUDX ladder
(read < create < update < delete < extra_risky) — fresh sessions see only
`read`-tier tools plus the always-available `system.info` and the four
`request_*_access` elevation tools. After a successful elevation, a
`tools/list_changed` notification fires so the client refetches the tool
list.

Run as:
    python /abs/path/to/mcp-server/server.py

Configured via env vars:
    REMOTE_HANDS_HOST           agent host (default 127.0.0.1)
    REMOTE_HANDS_PORT           agent TCP port (default 8765)
    REMOTE_HANDS_TOKEN_PATH     path to the agent's elevation token file
                                (default %ProgramData%\\AgentRemoteHands\\token)
    PROTOCOL_SPEC_DIR           path to the protocol-repo `spec/` directory.
                                If unset, the loader walks up from the bridge
                                module looking for `Protocol/spec/` or
                                `spec/` siblings.
"""

from __future__ import annotations

import asyncio
import os
import socket
import sys
import traceback
from typing import Any

# Make sibling modules importable when run as `python server.py`.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from agent_client import AgentClient, WireError  # noqa: E402
from tools import find_tool, tools_by_tier  # noqa: E402

import mcp.types as types  # noqa: E402
from mcp.server import NotificationOptions, Server  # noqa: E402
from mcp.server.models import InitializationOptions  # noqa: E402
from mcp.server.stdio import stdio_server  # noqa: E402


SERVER_NAME = "agent-remote-hands"
SERVER_VERSION = "0.3.0"


def _build_agent_client() -> AgentClient:
    host = os.environ.get("REMOTE_HANDS_HOST", "127.0.0.1")
    port = int(os.environ.get("REMOTE_HANDS_PORT", "8765"))
    # mdns:auto / mdns:select → resolve via on-LAN discovery before
    # connecting. See mcp-server/mdns_select.py.
    if host.startswith("mdns:"):
        from mdns_select import resolve, MdnsError
        try:
            host = resolve(host)
            print(
                f"[mcp-server] mDNS resolved to {host}:{port}",
                file=sys.stderr,
            )
        except MdnsError as e:
            print(f"[mcp-server] mDNS resolution failed: {e}", file=sys.stderr)
            raise SystemExit(2)
    return AgentClient(host=host, port=port, client_name=SERVER_NAME)


def _format_resolution_diagnostic(host: str, port: int) -> str:
    """Format a human-readable list of addresses `host` resolves to.

    Used in the connect-failure error message so the user can immediately
    see whether they're hitting an mDNS-IPv6-multi-prefix scenario rather
    than guessing whether the agent is up. See issue #65.
    """
    try:
        infos = socket.getaddrinfo(
            host, port, socket.AF_UNSPEC, socket.SOCK_STREAM)
    except socket.gaierror as e:
        return f"resolution failed: {e}"
    if not infos:
        return "no addresses resolved"
    pretty: list[str] = []
    for fam, _, _, _, sa in infos:
        addr = sa[0]
        if fam == socket.AF_INET6:
            pretty.append(f"[{addr}]")
        else:
            pretty.append(addr)
    return "resolved to: " + ", ".join(pretty)


async def _call_with_recovery(client: AgentClient, tool, args: dict) -> str:
    """Run a tool handler with bridge-side recovery on `WireError`.

    Policy:
    - Non-`WireError` exceptions are surfaced as-is (with a Python traceback).
      Handlers raise these for verb-level / argument-validation failures
      that the LLM can act on directly.
    - `WireError` on a read-only tool triggers auto-recovery: the bridge
      tears down and re-establishes the connection (``client.reset()``),
      then retries the handler once. On retry success the result is
      prefixed with a recovery note so the LLM knows a reset happened.
      Read-only is the strictest auto-retry policy — verbs that don't
      modify state are guaranteed safe to repeat. ``idempotent_hint``
      (the next-strictest annotation) means "repeating produces the same
      end state" but may still cause secondary effects (a double click,
      a duplicated keystroke), so it does NOT trigger auto-retry.
    - `WireError` on a non-read-only tool returns a structured wire_error
      message naming the failure and directing the LLM to call
      ``agent_reset``. This is the manual escape hatch path.

    Note that ``WireError`` traceback leak — the operator's complaint #78
    against the previous code that dumped 25 lines of Python stack — is
    fixed here as a side-effect. The traceback goes to stderr; the LLM
    sees a structured single-line summary.
    """
    name = tool.name
    try:
        return await asyncio.to_thread(tool.handler, args, client)
    except WireError as ex:
        # Server-side log retains the trace for the bridge developer; the
        # LLM-facing surface is the structured message below.
        print(f"[mcp-server] WireError in {name}: {ex}",
              file=sys.stderr)
        traceback.print_exc(file=sys.stderr)

        if not tool.read_only_hint:
            return (
                f"wire_error: {ex}\n"
                f"Tool `{name}` is not read-only, so the bridge did not "
                f"auto-retry — the call may have had side effects on the "
                f"agent before the framing broke. Call `agent_reset` with "
                f"a one-line reason to recover, then decide whether to "
                f"retry this call based on whether its side effects are "
                f"acceptable to repeat.")

        # Read-only: attempt one transparent reset + retry.
        reset = await asyncio.to_thread(client.reset)
        if not reset.get("ok"):
            return (
                f"wire_error: {ex}\n"
                f"Bridge attempted auto-recovery and failed: "
                f"{reset.get('error')!r} ({reset.get('detail')}). "
                f"The agent may be unreachable. Re-check connectivity, "
                f"then try `agent_reset` manually.")

        try:
            text = await asyncio.to_thread(tool.handler, args, client)
        except WireError as ex2:
            return (
                f"wire_error: retry-after-reset also failed: {ex2}. "
                f"The agent is reachable (reset succeeded) but framing "
                f"is breaking on every call. Likely an agent-side bug; "
                f"surface this to the bridge maintainer.")
        # Prefix the success path with a recovery note so the LLM has
        # the context that a reset just happened (relevant if the next
        # call depends on prior tier or subscription state).
        note = (
            "[bridge recovery: auto-reset succeeded after wire_error; "
            "any prior tier elevation and active watch.* subscriptions "
            "were lost. Re-elevate / re-subscribe if needed.]\n\n")
        return note + text
    except Exception as ex:  # noqa: BLE001 — surface other handler failures
        return f"Error in {name}: {ex}\n\n{traceback.format_exc()}"


async def main() -> None:
    client = _build_agent_client()
    try:
        # Block until we successfully hello the agent — anything else is a
        # configuration error worth surfacing immediately rather than after
        # the first tool call.
        await asyncio.to_thread(client.connect)
    except (OSError, WireError) as ex:
        diag = _format_resolution_diagnostic(client.host, client.port)
        norm_host = client.host.lower().rstrip(".")
        is_mdns = norm_host.endswith(".local")
        hint = ""
        if is_mdns:
            hint = (
                "\n  hint: on dual-stack networks, .local hostnames often "
                "return many IPv6 records ahead of A. agent_client.py "
                "prefers IPv4 for .local, but if your network's IPv6 "
                "routing is broken in a way that masks the bridge's "
                "preference, set REMOTE_HANDS_HOST to the IPv4 literal "
                "as a workaround. See issue #65."
            )
        print(
            f"[mcp-server] could not reach agent at {client.host}:{client.port}: {ex}\n"
            f"  {diag}{hint}",
            file=sys.stderr,
        )
        sys.exit(1)

    server: Server = Server(SERVER_NAME)

    @server.list_tools()
    async def handle_list_tools() -> list[types.Tool]:
        # Refresh tier from the agent on each list — cheap, and means the
        # surface stays in sync if some other connection raised tier on
        # the same agent (rare but possible).
        try:
            info = await asyncio.to_thread(client.info)
            client._current_tier = info.get("current_tier", client.current_tier)
        except WireError:
            pass  # fall back to last-known tier

        defs = tools_by_tier(client.current_tier)
        return [_to_mcp_tool(t.to_mcp_tool()) for t in defs]

    @server.call_tool()
    async def handle_call_tool(
        name: str, arguments: dict[str, Any] | None
    ) -> list[types.TextContent]:
        tool = find_tool(name)
        if tool is None:
            return [types.TextContent(
                type="text",
                text=f"Unknown tool: {name!r}. Call `system.info` to see what's available."
            )]
        if tool.tier != "always" and not client.can_satisfy(tool.tier):
            return [types.TextContent(
                type="text",
                text=(
                    f"Tool `{name}` requires the `{tool.tier}` tier; the "
                    f"connection is at `{client.current_tier}`. Call "
                    f"`request_{tool.tier}_access` with a one-line reason "
                    f"first."
                ),
            )]

        prior_tier = client.current_tier
        text = await _call_with_recovery(client, tool, arguments or {})

        # If the tier changed (an elevation tool just succeeded), tell the
        # client to refetch the tool list — that's how the LLM "sees" the
        # newly-available higher-tier tools.
        if client.current_tier != prior_tier:
            try:
                await server.request_context.session.send_tool_list_changed()
            except Exception:
                # Notification is best-effort — clients that don't support
                # listChanged just keep the current cache and miss the
                # broader surface until they refetch on their own schedule.
                pass

        return [types.TextContent(type="text", text=text)]

    init_options = InitializationOptions(
        server_name=SERVER_NAME,
        server_version=SERVER_VERSION,
        capabilities=server.get_capabilities(
            notification_options=NotificationOptions(tools_changed=True),
            experimental_capabilities={},
        ),
    )

    try:
        async with stdio_server() as (read_stream, write_stream):
            await server.run(read_stream, write_stream, init_options)
    finally:
        client.close()


def _to_mcp_tool(d: dict) -> types.Tool:
    """Inflate a plain dict (from ToolDef.to_mcp_tool) into the MCP
    `Tool` model. Kept thin so the dict shape is the source of truth."""
    return types.Tool(
        name=d["name"],
        description=d["description"],
        inputSchema=d["inputSchema"],
        annotations=types.ToolAnnotations(**d.get("annotations", {})) if d.get("annotations") else None,
    )


if __name__ == "__main__":
    asyncio.run(main())
