# mcp-server

Python MCP bridge that exposes the [Agent Remote Hands wire protocol](../PROTOCOL.md) as named MCP tools to LLM clients (Claude Code, Claude Desktop, etc.).

## What it does

- Holds **one TCP connection** to a running agent
- Exposes **tier-filtered MCP tools** on the v2.1 CRUDX ladder (`read` < `create` < `update` < `delete` < `extra_risky`): a fresh session sees only `read`-tier tools plus the always-available `system.info`, `evaluate_with_variants`, and four elevation tools (`request_create_access`, `request_update_access`, `request_delete_access`, `request_extra_risky_access`)
- The LLM elevates explicitly: it calls e.g. `request_update_access(reason="...")`, the bridge raises tier on the agent connection, and the next `tools/list` query exposes the broader surface
- Higher rungs subsume lower rungs per the ladder: raising to `update` covers `create`+`read`; raising to `delete` covers `update`+`create`+`read`; etc.
- **Schemas are spec-driven.** Tools whose wire verb has a `Repos/Protocol/spec/verbs/<verb>.json` entry load their `input_schema` from that file (set `PROTOCOL_SPEC_DIR` to override the search path).

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

The bridge auto-reads the token file when the LLM calls any `request_*_access` tool. The token must be readable to whatever user the bridge runs as — for cross-machine deployments, copy the file out-of-band.

| Env var | Default | Purpose |
|---|---|---|
| `REMOTE_HANDS_HOST` | `127.0.0.1` | Agent TCP host |
| `REMOTE_HANDS_PORT` | `8765` | Agent TCP port |
| `REMOTE_HANDS_TOKEN_PATH` | `%ProgramData%\AgentRemoteHands\token` | Path to the agent's elevation token file |
| `PROTOCOL_SPEC_DIR` | `<bridge>/../../Protocol/spec` | Path to the protocol-repo `spec/` directory used for schema lifts |

## The tier model

The wire-protocol tiers are a CRUDX ladder: `read` < `create` < `update` < `delete` < `extra_risky`. Holding a higher tier subsumes everything below.

| Tier | Tools the LLM sees | Examples |
|---|---|---|
| `always` | 6 | `system.info`, `evaluate_with_variants`, `request_create_access`, `request_update_access`, `request_delete_access`, `request_extra_risky_access` |
| `read` (default) | + 15 | `screen.capture`, `window.find`, `element.find`, `element.wait`, `file.wait`, `wait_for_visual_change`, `wait_for_window`, `wait_for_process_exit`, `wait_for_file_change`, `file.read`, `directory.list`, `window.list`, `element.list`, `process.list`, `clipboard.get` |
| `create` (after `request_create_access`) | + 4 | `directory.create`, `directory.rename`, `process.start`, `process.shell` |
| `update` (after `request_update_access`) | + 11 | `element.click`, `input.type`, `input.key`, `input.click`, `element.set_text`, `file.write`, `file.write_b64`, `file.upload`, `clipboard.set`, `window.focus`, `window.close` |
| `delete` (after `request_delete_access`) | + 3 | `process.kill`, `file.delete`, `directory.remove` |
| `extra_risky` (after `request_extra_risky_access`) | + 1 | `system.power.cancel` |

`request_create_access` exists for callers that only need to create things (`directory.create`, `process.start`). For most automation flows raising directly to `update` is the simpler choice — `update` subsumes `create` per the ladder.

Detailed tool descriptions are visible in any MCP client's tool inspector — call `system.info` first to see what the connected agent advertises.

## A typical session

The LLM driving an installer:

1. `system.info` → confirms `integrity=medium`, `uiaccess=false`, `current_tier=read`
2. `file.wait(pattern="C:\\Users\\…\\Downloads\\Firefox*.exe", timeout_ms=60000)` → installer download lands
3. `process.start(command_line="Firefox Installer.exe")` → fails: requires `create` tier
4. `request_update_access(reason="run downloaded Firefox installer")` → OK (raises to `update`, which subsumes `create`)
5. *(client refetches tools — update surface now visible)*
6. `process.start(command_line="Firefox Installer.exe")` → OK, returns PID
7. `element.wait(role="button", name="Install", timeout_ms=30000)` → element appears
8. `element.click(role="button", name_pattern="Install")` → wizard advances

If the wizard auto-elevates to High IL, step 8 returns `ERR uipi_blocked` instead — see [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels) for workarounds.

## Common pitfalls

- **`uipi_blocked` errors mean integrity-level mismatch**, not a bug. The agent runs at Medium IL by default; installer wizards auto-elevate to High IL. Either run a second elevated agent on a different port or sign the agent binary with `uiAccess="true"`. See [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels).
- **MSI installs serialise via a global Windows mutex** — don't parallelise. See [`docs/windows-automation-notes.md`](../docs/windows-automation-notes.md).
- **The bridge holds one connection** to the agent — tool calls serialise. For parallel automation, run multiple agent / bridge pairs.
- **Token file ACLs matter.** The bridge reads the token from the local filesystem; if the bridge and the agent run on different machines, copy the token via a trusted channel and point `REMOTE_HANDS_TOKEN_PATH` at the local copy.

## Subscription wait-fors (fire-once)

Four `wait_for_*` tools translate the agent's `watch.*` subscription verbs into synchronous MCP tool calls:

| MCP tool | Wraps | Use case |
|---|---|---|
| `wait_for_visual_change` | `watch.region --until-change` | Wait for any pixel change in a region. Returns the captured PNG (base64). |
| `wait_for_window` | `watch.window` | Wait for a top-level Win32 window with a matching title prefix to appear or disappear. |
| `wait_for_process_exit` | `watch.process` | Wait for a PID to exit; returns the exit code. Critical for installer / build flows after `launch`. |
| `wait_for_file_change` | `watch.file` | Wait for any file modification matching a path or glob. Different from `wait_for_file` (path-existence). |

All four are read-tier and read-only-hinted. The flow is: subscribe via the underlying `watch.*` verb, block on the bridge's connection waiting for the first matching EVENT (or timeout), cancel the subscription, return a structured tool result. Continuous-subscription scenarios (LLM wants every change) aren't yet supported — open an issue if you have a real use case.

The variant-family registry knows these tools as the "wait for X to happen" family — picking between them is a useful test-mode signal. Decision rule:

- UIA-visible target → `wait_for_element` (cheaper, semantic)
- Top-level Win32 dialog → `wait_for_window`
- Visual-only change (canvas / Unity / DirectComposition) → `wait_for_visual_change`
- Process completion → `wait_for_process_exit`
- File system change → `wait_for_file_change`

## Test mode — `evaluate_with_variants`

The bridge ships a meta-tool, `evaluate_with_variants`, that an LLM uses **in place of a direct tool call** when running in benchmark / evaluation mode. The flow:

1. LLM submits `{preferred: {tool, arguments}, variants: [{tool, arguments}, {tool, ruled_out_reason}, ...]}`.
2. Bridge runs the preferred and any non-ruled-out variants serially via the existing connection.
3. Bridge consults the variant-family registry ([`variant_families.py`](variant_families.py)) — if the preferred tool has registered alternatives the LLM didn't address (neither as a variant nor with `ruled_out_reason`), the response includes a `coverage_gap` field naming the missing tools and their registry-suggested reasons.
4. LLM acts on the preferred result. On its next call, it should either include the missing variants with proposed args, or include them with `ruled_out_reason` documenting why.

The bridge is forcing exploration: the LLM has to either *try* every registered alternative or *justify* ruling each one out. The accumulated `ruled_out_reason` text becomes high-quality signal for `LLM-OPERATORS.md`'s decision tree — consistent rulings-out are documentation candidates.

### Variant-family registry

Hand-curated map of MCP tool name → related tools that should be considered as alternatives. Lives in [`variant_families.py`](variant_families.py). Update as part of any tool-shipping PR. Three current consumers (the second and third are tracked but not yet shipped):

- `evaluate_with_variants` (this) — coverage-gap detection
- (Future) Production loop-detection coaching — append `loop_advisory` when an LLM is repeatedly calling the same tool without success
- (Doc-time) [`LLM-OPERATORS.md`](../LLM-OPERATORS.md) decision tree — reasons text in the registry doubles as the source for the operator-facing decision tree

### When to use

- **Always**, when running in test-mode benchmarks. The whole point is to push the LLM toward variants it would otherwise rule out without thinking.
- **Never** for routine production use — the meta-tool adds round-trip overhead and is only useful for evaluation.

The tool is in the `always` tier so it's available without raising. Variants run at whatever tier they require — if a variant needs `update` and the connection is at `read`, the variant is reported as not-triggered with a tier-required reason; the LLM should `request_update_access` and retry.

### Worked example

LLM driving a benchmark, considering `find_element` to locate a button:

```json
{
  "preferred": {
    "tool": "find_element",
    "arguments": {"role": "Button", "name_pattern": "BEGIN"}
  },
  "variants": [
    {
      "tool": "wait_for_element",
      "arguments": {"role": "Button", "name_pattern": "BEGIN", "timeout_ms": 2000}
    },
    {
      "tool": "find_window",
      "ruled_out_reason": "looking for a button inside the current window, not a separate dialog window"
    },
    {
      "tool": "list_elements",
      "ruled_out_reason": "I know the role and name; enumeration is overkill"
    }
  ]
}
```

Bridge runs `find_element` and `wait_for_element` (both at read tier), returns:

```json
{
  "kind": "evaluate_with_variants_result",
  "preferred": {
    "tool": "find_element",
    "result": "...",
    "triggered": true,
    "elapsed_ms": 12
  },
  "variants": [
    {"tool": "wait_for_element", "result": "...", "triggered": true, "elapsed_ms": 14},
    {"tool": "find_window", "ruled_out": true, "reason": "looking for a button inside the current window, not a separate dialog window"},
    {"tool": "list_elements", "ruled_out": true, "reason": "I know the role and name; enumeration is overkill"}
  ],
  "feedback_hint": "If you'd like to provide reflective feedback..."
}
```

No `coverage_gap` — every registered family member was addressed.

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
4. Ask the LLM to call `system.info` — you should see the agent's `system.info` JSON
