# mcp-server

Python MCP bridge that exposes the [Agent Remote Hands wire protocol](../PROTOCOL.md) as named MCP tools to LLM clients (Claude Code, Claude Desktop, etc.).

## What it does

- Holds **one TCP connection** to a running agent
- Exposes **tier-filtered MCP tools**: a fresh session sees only `observe`-tier tools (read-only) plus three always-available tools (`agent_info`, `request_drive_access`, `request_power_access`)
- The LLM elevates explicitly: it calls `request_drive_access(reason="...")`, the bridge raises tier on the agent connection, and the next `tools/list` query exposes write / input tools
- `request_power_access` similarly unlocks destructive tools (file delete, process kill, shutdown)

The intent: the LLM **decides** when to take the safety brakes off, with a logged `reason`. The bridge can't autonomously do anything destructive without that step.

## Install

```bash
pip install -r mcp-server/requirements.txt
```

The bridge depends on the [`mcp` Python SDK](https://github.com/modelcontextprotocol/python-sdk). Python 3.10+.

## Configure

Drop a `.mcp.json` in your project root:

```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "python",
      "args": ["/abs/path/to/mcp-server/server.py"],
      "env": {
        "REMOTE_HANDS_HOST":       "<vm-host>",
        "REMOTE_HANDS_PORT":       "8765",
        "REMOTE_HANDS_TOKEN_PATH": "C:\\ProgramData\\AgentRemoteHands\\token"
      }
    }
  }
}
```

The bridge auto-reads the token file when the LLM calls `request_drive_access` / `request_power_access`. The token must be readable to whatever user the bridge runs as — for cross-machine deployments, copy the file out-of-band.

| Env var | Default | Purpose |
|---|---|---|
| `REMOTE_HANDS_HOST` | `127.0.0.1` | Agent TCP host |
| `REMOTE_HANDS_PORT` | `8765` | Agent TCP port |
| `REMOTE_HANDS_TOKEN_PATH` | `%ProgramData%\AgentRemoteHands\token` | Path to the agent's elevation token file |

## The tier model

| Tier | Tools the LLM sees | Examples |
|---|---|---|
| `always` | 3 | `agent_info`, `request_drive_access`, `request_power_access` |
| `observe` (default) | + 11 | `take_screenshot`, `find_window`, `find_element`, `wait_for_element`, `wait_for_file`, `read_file`, `list_directory`, `list_windows`, `list_elements`, `list_processes`, `get_clipboard` |
| `drive` (after `request_drive_access`) | + 10 | `click_element`, `type_text`, `press_keys`, `click`, `set_element_text`, `write_file`, `launch`, `shell_open`, `focus_window`, `close_window` |
| `power` (after `request_power_access`) | + 3 | `kill_process`, `delete_file`, `cancel_pending_shutdown` |

Detailed tool descriptions are visible in any MCP client's tool inspector — call `agent_info` first to see what the connected agent advertises.

## A typical session

The LLM driving an installer:

1. `agent_info` → confirms `integrity=medium`, `uiaccess=false`, `current_tier=observe`
2. `wait_for_file("C:\\Users\\…\\Downloads\\Firefox*.exe", 60000)` → installer download lands
3. `launch("Firefox Installer.exe")` → fails: requires `drive` tier
4. `request_drive_access(reason="run downloaded Firefox installer")` → OK
5. *(client refetches tools — drive surface now visible)*
6. `launch("Firefox Installer.exe")` → OK, returns PID
7. `wait_for_element(role="button", name_pattern="Install", timeout_ms=30000)` → element appears
8. `click_element(role="button", name_pattern="Install")` → wizard advances

If the wizard auto-elevates to High IL, step 8 returns `ERR uipi_blocked` instead — see [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels) for workarounds.

## Common pitfalls

- **`uipi_blocked` errors mean integrity-level mismatch**, not a bug. The agent runs at Medium IL by default; installer wizards auto-elevate to High IL. Either run a second elevated agent on a different port or sign the agent binary with `uiAccess="true"`. See [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels).
- **MSI installs serialise via a global Windows mutex** — don't parallelise. See [`docs/windows-automation-notes.md`](../docs/windows-automation-notes.md).
- **The bridge holds one connection** to the agent — tool calls serialise. For parallel automation, run multiple agent / bridge pairs.
- **Token file ACLs matter.** The bridge reads the token from the local filesystem; if the bridge and the agent run on different machines, copy the token via a trusted channel and point `REMOTE_HANDS_TOKEN_PATH` at the local copy.

## What's covered today

This is a starter set. The current tool surface covers 27 of the agent's 66 wire verbs — the most useful ones for typical automation flows. Verbs not yet wrapped (e.g. `watch.*` subscriptions, `registry.*` reads/writes, `window.move`, `window.state`, `element.tree`, `element.toggle`/`expand`/`collapse`, `process.wait`, `system.lock`/`hibernate`/`sleep`, etc.) can be added in [`tools.py`](tools.py); the dispatch table at the bottom is the only place that changes.

Issues to follow:

- [#11](https://github.com/WilliamIsted/agent-remote-hands/issues/11) — `click_element` accepts explicit `element_id` (shipped — see the tool's three call shapes)
- [#32](https://github.com/WilliamIsted/agent-remote-hands/issues/32) — tier-aware tool surface (shipped — this README is the docs)

## Tests

A pytest smoke harness ships alongside the bridge — runs against an in-process
mock agent so it works on any platform without needing a live Windows host:

```bash
pip install pytest
cd mcp-server
python -m pytest tests/
```

The harness exercises wire framing (`agent_client.py`), tier transitions, and
the tool registry's tier filtering / dispatch / annotation shape. 21 tests,
sub-second wall clock.

For end-to-end verification against a real agent:

1. Run an agent on a Windows host: `remote-hands.exe --discoverable`
2. Set `REMOTE_HANDS_HOST` to that host
3. Wire up `.mcp.json` and start an LLM session — the bridge starts on demand
4. Ask the LLM to call `agent_info` — you should see the agent's `system.info` JSON
