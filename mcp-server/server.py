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
current connection tier — fresh sessions see only `observe`-tier tools
plus the always-available `agent_info` / `request_drive_access` /
`request_power_access`. After a successful elevation, a `tools/list_changed`
notification fires so the client refetches the tool list.

Run as:
    python /abs/path/to/mcp-server/server.py

Configured via env vars:
    REMOTE_HANDS_HOST           agent host (default 127.0.0.1)
    REMOTE_HANDS_PORT           agent TCP port (default 8765)
    REMOTE_HANDS_TOKEN_PATH     path to the agent's elevation token file
                                (default %ProgramData%\\AgentRemoteHands\\token)
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
SERVER_VERSION = "0.2.0"


def _build_agent_client() -> AgentClient:
    host = os.environ.get("REMOTE_HANDS_HOST", "127.0.0.1")
    port = int(os.environ.get("REMOTE_HANDS_PORT", "8765"))
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
                text=f"Unknown tool: {name!r}. Call `agent_info` to see what's available."
            )]
        if tool.tier != "always" and not client.can_satisfy(tool.tier):
            return [types.TextContent(
                type="text",
                text=(
                    f"Tool `{name}` requires the `{tool.tier}` tier; the "
                    f"connection is at `{client.current_tier}`. Call "
                    f"`request_{'drive' if tool.tier == 'drive' else 'power'}_access` "
                    f"with a one-line reason first."
                ),
            )]

        prior_tier = client.current_tier
        try:
            text = await asyncio.to_thread(
                tool.handler, arguments or {}, client)
        except Exception as ex:  # noqa: BLE001 — surface any handler failure
            text = f"Error in {name}: {ex}\n\n{traceback.format_exc()}"

        # If the tier changed (an elevation tool just succeeded), tell the
        # client to refetch the tool list — that's how the LLM "sees" the
        # newly-available drive / power tools.
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
