# Agent Remote Hands — MCP server

Bridges Claude Code (and other MCP-aware clients) to the Agent Remote Hands binary running inside a target machine. Translates MCP tool calls into wire-protocol commands; gates the visible tool set by what the agent actually supports; nudges the model toward semantic actions over pixel actions.

## Install

```bash
pip install -e mcp-server/
```

Or run directly without installing:

```bash
python mcp-server/server.py
```

## Configure Claude Code

Add an entry to `~/.claude/claude_desktop_config.json` (Mac/Linux) or `%APPDATA%\Claude\claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "agent-remote-hands-mcp",
      "env": {
        "REMOTE_HANDS_HOST": "192.168.1.42",
        "REMOTE_HANDS_PORT": "8765"
      }
    }
  }
}
```

Or, if running from source without installing:

```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "python",
      "args": ["/abs/path/to/repo/mcp-server/server.py"],
      "env": {
        "REMOTE_HANDS_HOST": "192.168.1.42"
      }
    }
  }
}
```

Don't know the agent's IP? Run `client/hostctl-discover` from the same LAN; the agent must have been started with `REMOTE_HANDS_DISCOVERABLE=1`.

## How it conveys preferences

Claude Code's tool choice is shaped here through four layered mechanisms:

1. **Tool descriptions** name preferred alternatives explicitly. `click(x, y)`'s description starts with "PREFER `click_element` when targeting a named UI control."
2. **Cross-tool guidance** is woven into descriptions throughout — every "lower-tier" tool points at its preferred alternative.
3. **Server-level `instructions`** set the policy for the whole session: semantic over mechanical, UIA before pixels, `wait_for_change` over polling, verify after state changes.
4. **Tool annotations** (`destructiveHint`, `readOnlyHint`) flag destructive operations (`delete_file`, `kill_process`, `lock_workstation`) so Claude Code surfaces confirmations before invoking.

## Tool surface

Tools are registered dynamically based on the connected agent's `CAPS` and `INFO`. A `windows-modern` agent with full UIA exposes ~30 tools; a stripped-down `windows-nt` agent (no UIA, no WGC) sees a smaller subset. The model never sees a tool the agent can't run.

## Security note

The MCP server has no authentication of its own — it inherits Claude Code's local-user trust boundary. The underlying agent also has no auth (its trust boundary is the network). Combined: only point the MCP server at agents you trust. Don't expose the agent on public networks.
