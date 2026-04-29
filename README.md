# Agent Remote Hands

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Built for Windows](https://img.shields.io/badge/built%20for-Windows%2010%2B-0078D4?logo=windows&logoColor=white)](agents/windows-modern/COMPATIBILITY.md)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](agents/windows-modern/CMakeLists.txt)
[![Status: pre-release](https://img.shields.io/badge/status-pre--release-orange)](https://github.com/WilliamIsted/agent-remote-hands/milestones)
[![CodeQL](https://github.com/WilliamIsted/agent-remote-hands/actions/workflows/codeql.yml/badge.svg)](https://github.com/WilliamIsted/agent-remote-hands/actions/workflows/codeql.yml)
[![Last commit](https://img.shields.io/github/last-commit/WilliamIsted/agent-remote-hands?logo=github)](https://github.com/WilliamIsted/agent-remote-hands/commits/main)
[![MCP-ready](https://img.shields.io/badge/MCP-ready-7B68EE)](https://modelcontextprotocol.io/)
[![Built with Claude](https://img.shields.io/badge/built%20with-Claude%20Code-D97757)](https://claude.com/code)

A small TCP agent that gives AI coding agents remote hands on Windows machines — mouse, keyboard, screenshots, process control, file I/O, UI Automation, window management — over a single line-oriented socket.

Built for [Claude Code](https://docs.claude.com/en/docs/claude-code) and other MCP-aware LLM clients to drive virtual or physical Windows machines. Multi-target by design: the modern build (Windows 10 / 11) ships today; a planned legacy build (Windows NT through Server 2003) will share the same wire protocol so the same controller speaks to both.

## Why

When an LLM needs to install software, configure a system, or verify a deliverable on Windows, it usually has two options — neither great:

- **Hosted Computer Use APIs** (pixel-based clicks, no introspection, slow round trips)
- **PowerShell over WinRM** (no GUI awareness, blind to dialogs and modals, lands you in non-interactive Session 0)

Agent Remote Hands sits between those: a native agent on the target exposes a wire protocol that Claude — or any MCP client — can drive directly, with **rich introspection** (UI Automation tree, window list, process table, registry, screenshots) and **low-latency control** (synthetic keyboard/mouse, foreground focus, sub-100 ms screenshots) over plain TCP.

## Quick start

Build the agent (from a Developer PowerShell with CMake on PATH):

```powershell
cmake -S agents/windows-modern -B agents/windows-modern/build -A x64
cmake --build agents/windows-modern/build --config Release
```

Install on the target Windows machine (Administrator PowerShell):

```powershell
# Step 1: configure Defender exclusion BEFORE the binary lands
.\Tools\install-agent.ps1 -PrepareDefender

# Step 2: drop remote-hands.exe into C:\Program Files\AgentRemoteHands\
#         (drag-drop, RDP file paste, scp, etc — any path that lands it
#         directly in the now-excluded directory)

# Step 3: complete the install
.\Tools\install-agent.ps1 -Discoverable
```

Or, if you can fetch the binary by URL (e.g. from a release asset), one-shot:

```powershell
.\Tools\install-agent.ps1 -SourceUrl 'https://example/remote-hands.exe' -Discoverable
```

The script adds a Microsoft Defender exclusion for `%ProgramFiles%\AgentRemoteHands\`, places the binary there, adds binary-scoped Windows Firewall rules (TCP/8765 + UDP/5353 for mDNS when `-Discoverable`), and registers a Task Scheduler logon-task with restart-on-failure so the agent autostarts in the user's interactive desktop session on next logon. `-Uninstall` reverses everything (including the Defender exclusion).

### Why the Defender exclusion?

Microsoft Defender's machine-learning heuristic flags any unsigned remote-control tool of this shape (synthetic input + screen capture + arbitrary file I/O + process kill + TCP listener) as `Program:Win32/Contebrew.A!ml` — regardless of the binary's specific build, install behaviour, or strings present. The detection is structural. Without a code-signing certificate, the only mitigation is to deploy into a path Defender doesn't scan. Code signing is tracked as the v1.0 GA blocker.

Sanity-check from another machine using the conformance suite:

```bash
pip install pytest
python tests/conformance/run.py <vm-host>
```

To wire it into Claude Code (once `mcp-server/` lands — see the Roadmap), drop a `.mcp.json` into your project root:

```json
{
  "mcpServers": {
    "agent-remote-hands": {
      "command": "python",
      "args": ["/abs/path/to/mcp-server/server.py"],
      "env": {
        "REMOTE_HANDS_HOST":        "<vm-host>",
        "REMOTE_HANDS_PORT":        "8765",
        "REMOTE_HANDS_GUIDANCE":    "hint",
        "REMOTE_HANDS_TIER_POLICY": "ask"
      }
    }
  }
}
```

`REMOTE_HANDS_*` env vars set the project default for each setting; the LLM (or you, in natural language) can override most of them mid-session via MCP tools — see [#55](https://github.com/WilliamIsted/agent-remote-hands/issues/55) for the settings-registry pattern.

## What's in the repo

| Path | Purpose |
|---|---|
| [`PROTOCOL.md`](PROTOCOL.md) | v2 wire-protocol spec — length-prefixed framing, structured errors, three-tier permission model, 66 verbs across 11 namespaces |
| [`docs/windows-automation-notes.md`](docs/windows-automation-notes.md) | Operational gotchas — MSI mutex, Session 0 isolation, foreground lock policy, integrity-level pitfalls |
| [`agents/windows-modern/`](agents/windows-modern/) | Windows 10 / 11 agent — C++17, IUIAutomation, WIC for PNG, hand-rolled mDNS responder. Built with CMake. |
| [`tests/conformance/`](tests/conformance/) | Python pytest suite — capability-gated tests across all 11 namespaces; runs against any agent that speaks the protocol |
| [`agents/windows-modern/tests/unit/`](agents/windows-modern/tests/unit/) | doctest unit tests for the pure-logic modules (framing, JSON, error codes, tier model) |
| `agents/windows-nt/` | *(planned)* Legacy agent (NT 4 → Server 2003) — straight C, WinSock, GDI BitBlt |
| [`mcp-server/`](mcp-server/) | Python MCP bridge — exposes wire verbs as named tools to MCP-aware clients (Claude Code, Claude Desktop, …) with tier-aware tool filtering |
| `client/hostctl` | *(planned)* Reference Python CLI |
| `client/hostctl-discover` | *(planned)* mDNS LAN scanner |
| [`Tools/install-agent.ps1`](Tools/install-agent.ps1) | PowerShell installer — copies the binary to `%ProgramFiles%`, adds binary-scoped firewall rules, registers a Task Scheduler logon-task with restart-on-failure. `-Uninstall` reverses it. |
| `examples/vagrant/` | *(planned)* Win11 dev fixture for VirtualBox / VMware / Hyper-V |

## Architecture

Three layers:

1. **Agent** — a single binary per target OS family. Listens on TCP `8765` (configurable). Runs in the user's interactive desktop session via Task Scheduler logon-task autostart so it can drive the visible UI.
2. **Wire protocol** — length-prefixed framing over plain TCP. 66 verbs across 11 namespaces (`screen`, `window`, `input`, `element`, `file`, `process`, `registry`, `clipboard`, `system`, `watch`, `connection`). Three-tier permission model (`observe` / `drive` / `power`) with file-token elevation. Subscription-based streaming via `watch.*` verbs and out-of-band `EVENT` frames. Every agent advertises its capability set in `system.info` and `system.capabilities` so clients negotiate features without breaking on older or restricted builds.
3. **MCP server (or any client)** *(planned)* — bridges the wire protocol to a higher-level interface. The Python MCP server will expose named tools to Claude Code, filter them by what the agent advertises, and use description copy + MCP annotations to nudge the model toward semantic actions (`click_element` over `click(x,y)`, `find_element` over OCR).

Connections are per-thread, capped at the agent's advertised `max_connections` (default 4). `watch.*` subscriptions hold one connection for their duration; clients open side connections for interleaved commands.

## Discovery

When started with `REMOTE_HANDS_DISCOVERABLE=1` (or `--discoverable`), the agent advertises itself on the LAN via mDNS / DNS-SD as `_remote-hands._tcp.local.`. Any DNS-SD browser will see it; a dedicated Python scanner is planned (see Roadmap).

**The protocol has no built-in authentication today**, so discovery is opt-in per deployment — advertising on an untrusted network is a footgun. v3.0 of the agent (see Roadmap) introduces SSPI authentication; until then, advertise only on trusted networks and consider firewalling 8765 to known clients.

## Conformance suite

`tests/conformance/` runs against any agent on any platform. Each module exercises one namespace and gates by the agent's advertised capabilities — agents that don't implement a verb don't fail, they get the test skipped.

```bash
python tests/conformance/run.py <agent-host> 8765
```

The suite is the executable contract: anything passing it speaks the wire protocol correctly.

## Roadmap

Three architectural increments tracked as GitHub milestones:

| Milestone | Theme | Notes |
|---|---|---|
| **v1.0** | Stable protocol + per-connection tier system + agent-feedback fixes | Most issues live here. Adds `observe` / `drive` / `power` tiers gated by `connection.tier_raise`, file-token auth, and a backlog of observability + ergonomics improvements surfaced by real LLM driving sessions. Single-process. |
| **v2.0** | Privsep dispatcher | Privileged dispatcher process + tier-restricted worker processes. OS-enforced separation: the kernel refuses out-of-tier operations regardless of agent code paths. Compromise containment. |
| **v3.0** | SSPI auth + caller impersonation | Per-connection workers spawned under the authenticated caller's Windows identity. Filesystem and registry ACLs match the caller's permissions, not the agent's. Full WinRM-style auth model. |

[Open issues by milestone →](https://github.com/WilliamIsted/agent-remote-hands/milestones)

## Contributing

Issues and PRs are welcome. Useful labels:

- `agent-feedback` — surfaced by an LLM agent using the tool in real tasks
- `agent-authored` — issue body or PR text written by an LLM (provenance marker)
- `high-priority` / `low-priority` — direct cost vs. polish (no priority label = medium)
- standard `bug` / `enhancement` / `documentation`

When filing a wire-protocol change, please update `PROTOCOL.md`, add a conformance test, and run the suite against at least one target build before submitting.

## On authorship

The "Built with Claude Code" badge at the top is honest — a substantial portion of the source code, the v2 wire-protocol spec, and the test suites was drafted by Claude Code under direction. What the badge doesn't capture is the iteration: the architectural decisions, security-model trade-offs, naming conventions, and "wait, this is wrong" course-corrections that shaped the result. Those are human work, and there are many more hours of them than there are of "Claude wrote this and shipped it."

Treat the project as collaborative output, not autonomous agent work. Bug reports, architectural critiques, and PRs are welcome.

## License

Licensed under the Apache License, Version 2.0 (the "License"); you may not use the files in this repository except in compliance with the License. You may obtain a copy of the License at <http://www.apache.org/licenses/LICENSE-2.0>, or in the [`LICENSE`](LICENSE) file at the repo root.

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.

Copyright 2026 William Isted and contributors.
