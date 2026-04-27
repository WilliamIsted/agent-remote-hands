"""MCP server for Agent Remote Hands.

Bridges Claude Code (and any MCP-aware client) to the agent binary running
on the target machine. Translates tool calls to wire-protocol commands;
gates the visible tool surface by what the connected agent advertises in
CAPS and INFO; nudges the model toward semantic tools over pixel ones via
the four preference-conveying mechanisms documented in README.md.

Run as:
    REMOTE_HANDS_HOST=192.168.x.x python server.py

Or wire into Claude Code via ~/.claude/claude_desktop_config.json — see
README.md.
"""
from __future__ import annotations

import asyncio
import os
import sys

# Make sibling files importable when run directly (without `pip install`).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool

from agent_client import AgentError, AsyncAgentClient
from tools import ALL_TOOLS, ToolDef, filter_tools


# ---------------------------------------------------------------------------
# Server-level instructions — mechanism #3 from the preference design.
# Pre-pended to the model's context for the entire session; sets the global
# tool-choice policy. Concise on purpose: the model reads this once and
# treats it as the senior-engineer voice.
# ---------------------------------------------------------------------------

INSTRUCTIONS = """\
You are driving a Windows machine through a remote-hands agent. Tools are
organised by preference — choose semantic over mechanical, structural over
pixel-based.

PREFER FIRST — UI Automation (when these tools appear):
  - find_element / click_element / set_element_text — robust to DPI,
    theming, layout shifts, and language localisation. Use whenever the
    target is a named UI control (buttons, menu items, links, edit fields).
  - get_element_text — read field contents directly. More reliable than
    OCR'ing a screenshot.

PREFER SECOND — window-targeted:
  - focus_window / move_window / list_windows for whole top-level windows.

LAST RESORT — pixel-based:
  - click(x, y) / type_text / press_keys — use only when no UI element
    exists: drawing canvases, in-browser DOM content, custom-drawn
    controls, games and full-screen Direct3D apps.

For waiting / observing:
  - wait_for_change(timeout_ms) — block until the screen changes by more
    than a threshold, return the changed frame. Use whenever you're
    waiting for a UI event.
  - DO NOT poll take_screenshot in a loop.

For data transfer:
  - For large strings / paths / code blocks into a target app: prefer
    set_clipboard then press_keys('ctrl-v') over typing the text out.

Verification:
  - After any state-changing action where landing isn't certain, take a
    screenshot or read the affected element to confirm. Especially after
    `click(x, y)` and `type_text`, where landing depends on focus state
    and pixel coordinates.

Destructive operations (delete_file, kill_process, lock_workstation,
close_window, set_clipboard, write_file, run, launch) are flagged in
their schemas — Claude Code will surface confirmations. Reflect on
whether each is necessary before invoking.

If a tool returns ERR, read the message — it usually says what went
wrong (`window not found`, `not invokable`, `id` for a stale element id,
`busy` if the agent is at its connection cap, `aborted` if a parallel
caller cancelled).
"""


# ---------------------------------------------------------------------------
# Single shared connection to the agent. A connection is a serialized
# command channel — calls take the lock so two tool calls don't interleave
# their bytes on the wire. Multi-connection / pool support is a TODO.
# ---------------------------------------------------------------------------

class AgentSession:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.client: AsyncAgentClient | None = None
        self.caps: set[str] = set()
        self.info: dict[str, str] = {}
        self.lock = asyncio.Lock()

    async def ensure(self) -> AsyncAgentClient:
        """Lazy-connect, lazy-query CAPS/INFO. Reconnects after any
        connection drop. Caller must hold self.lock."""
        if self.client is not None and self.client.connected:
            return self.client
        if self.client is not None:
            await self.client.disconnect()
        self.client = AsyncAgentClient(self.host, self.port)
        await self.client.connect()
        self.caps = await self.client.query_caps()
        self.info = await self.client.query_info()
        return self.client

    async def close(self) -> None:
        if self.client is not None:
            await self.client.disconnect()
            self.client = None


# ---------------------------------------------------------------------------
# MCP server wiring
# ---------------------------------------------------------------------------

def to_mcp_tool(t: ToolDef) -> Tool:
    """Convert our ToolDef → MCP SDK Tool."""
    return Tool(
        name=t.name,
        description=t.description,
        inputSchema=t.schema,
        annotations=t.annotations or None,
    )


async def _run_server(host: str, port: int) -> None:
    session = AgentSession(host, port)
    server: Server = Server(name="agent-remote-hands", instructions=INSTRUCTIONS)

    # Bind a `tool_def_for(name)` lookup the dispatch handler can use.
    by_name: dict[str, ToolDef] = {t.name: t for t in ALL_TOOLS}

    @server.list_tools()
    async def list_tools_handler() -> list[Tool]:
        # Probe the agent on first call so the tool list is gated by the
        # actual agent's CAPS/INFO. If the agent isn't reachable, we return
        # an empty list — Claude Code will surface this as "no tools yet"
        # and the user can retry once the VM is up.
        async with session.lock:
            try:
                await session.ensure()
            except (OSError, AgentError):
                return []
            avail = filter_tools(session.caps, session.info)
        return [to_mcp_tool(t) for t in avail]

    @server.call_tool()
    async def call_tool_handler(name: str, arguments: dict | None):
        tool = by_name.get(name)
        if tool is None:
            raise ValueError(f"unknown tool: {name}")
        async with session.lock:
            client = await session.ensure()
            # Capability re-check after re-connect — agent may have changed.
            if tool.requires_caps and not tool.requires_caps.issubset(session.caps):
                missing = tool.requires_caps - session.caps
                raise ValueError(
                    f"tool '{name}' needs verbs the agent doesn't expose: "
                    f"{','.join(sorted(missing))}"
                )
            try:
                return await tool.handler(client, arguments or {})
            except AgentError as e:
                raise ValueError(str(e))

    async with stdio_server() as (read, write):
        await server.run(
            read,
            write,
            initialization_options=server.create_initialization_options(),
        )

    await session.close()


def main() -> int:
    host = os.environ.get("REMOTE_HANDS_HOST")
    port = int(os.environ.get("REMOTE_HANDS_PORT", "8765"))
    if not host:
        sys.stderr.write(
            "agent-remote-hands-mcp: REMOTE_HANDS_HOST not set. Either export "
            "the env var or set it in your Claude Code MCP config. Use "
            "`client/hostctl-discover` to find advertised agents on the LAN.\n"
        )
        return 1
    asyncio.run(_run_server(host, port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
