# CLAUDE.md

Operational guidance for Claude Code (claude.ai/code) working in this repo. The user-facing overview lives in [`README.md`](README.md); the wire-protocol spec lives in [`PROTOCOL.md`](PROTOCOL.md). This file complements both rather than duplicating them.

## What this repo is

A multi-binary Windows control surface for AI agents. Three top-level deliverables:

- **`agents/windows-modern/`** — C++17 agent for Windows 10 / 11. IUIAutomation, BitBlt + WIC for screen capture, hand-rolled mDNS responder. Built with CMake. **Shipped today.**
- **`agents/windows-nt/`** *(planned)* — Straight C agent for Windows NT 4 → Server 2003. WinSock, GDI BitBlt, classic input APIs. Built with `build.bat` (cl.exe).
- **`mcp-server/`** *(planned)* — Python MCP bridge that exposes the wire protocol as named tools to MCP-aware clients.

All targets speak the same wire protocol (`PROTOCOL.md`). The conformance suite (`tests/conformance/`) is the contract — anything passing it speaks the protocol correctly.

## Build

Windows 10 / 11 agent (from a Developer PowerShell):

```powershell
cmake -S agents/windows-modern -B agents/windows-modern/build -A x64
cmake --build agents/windows-modern/build --config Release
```

Output: `agents/windows-modern/build/Release/remote-hands.exe`.

Legacy NT agent *(planned)* — when the `agents/windows-nt/` target lands, it will build via:

```cmd
cd agents\windows-nt
build.bat
```

Output: `agents/windows-nt/remote-hands-nt.exe` (per the binary naming convention below).

## Conformance suite

The contract. Run against any host that speaks the protocol:

```bash
python tests/conformance/run.py <host> 8765
```

One pytest module per namespace, with capability-gated skipping — verbs the agent doesn't advertise get the test skipped, not failed.

When adding a new verb: add a test under `tests/conformance/test_<namespace>.py` and run the suite against any agent that advertises it before submitting.

## Vagrant dev fixture *(planned)*

When `examples/vagrant/` lands, it'll bring up a clean Win11 VM with the agent installed:

```bash
cd examples/vagrant
vagrant up
```

Useful for reproducing UIPI / UAC behaviour without polluting your dev machine. Until then, run the agent on a real Windows host or your own VM.

## Smoke testing

The conformance suite is the canonical way to exercise a live agent — it covers every namespace with capability-gated skips:

```bash
python tests/conformance/run.py <vm-host> 8765
```

Quick one-shot verb checks via `client/hostctl` are *(planned)* — until that ships, the conformance suite (or `tests/conformance/wire.py` from a Python REPL) is the way in.

## Branch model

- **`main`** — canonical. Everything ships here.
- **`benchmark`** — branch holding tool-benchmarking and comparison rigs (not part of the product). **Do not merge into `main`.** Treat as throwaway.

`benchmarks/` is in `.gitignore` for the same reason — anything written there is fixture, not feature.

## Conventions

- Commit messages: short, bullet-pointed, present-tense. Mirror existing log style.
- Branch off `main`; PRs target `main`.
- Markdown for all docs; prefer `.md` over `.rst` / inline HTML.
- Wire-protocol changes always touch three places: `PROTOCOL.md`, the agent source, and a conformance test. PRs missing any of the three are incomplete.

## Labels

Configured on the GitHub repo:

| Label | Meaning |
|---|---|
| `agent-feedback` | Surfaced by an LLM agent using the tool in real tasks |
| `agent-authored` | Issue body or PR text written by an LLM (provenance marker) |
| `high-priority` | Direct cost — UX hit, observability gap, blocked workflows |
| `low-priority` | Polish — only worth doing if cheap |
| `bug` / `enhancement` / `documentation` | Standard |

No `medium-priority` — absence of a high/low label means medium.

## Milestones

| Milestone | Theme |
|---|---|
| `v1.0` | Stable protocol + per-connection tier system + agent-feedback fixes |
| `v2.0` | Privsep dispatcher (privileged dispatcher + tier-restricted workers) |
| `v3.0` | SSPI auth + caller impersonation (per-connection workers under the caller's identity) |

Most agent-surfaced asks live in v1.0. v2.0 and v3.0 are architectural increments that change the security model, not features.

## Filing issues with `gh`

Standard issue:

```bash
gh issue create \
  --title "ELEMENT_FIND distinguishes 'not found' from 'UIA blind across IL'" \
  --body-file .github/issue-body.md \
  --label "agent-feedback,agent-authored,enhancement,high-priority" \
  --milestone "v1.0"
```

Native sub-issue link (the GitHub REST endpoint, not just a body reference):

```bash
gh api repos/<owner>/<repo>/issues/<parent>/sub_issues \
  -X POST \
  -F sub_issue_id=<child_issue_id>
```

Two gotchas: use `repos/` without a leading slash (Git Bash on Windows rewrites a leading `/` as a filesystem path), and use `-F sub_issue_id=...` not `-f` (the endpoint wants an integer).

## Common tasks

### Adding a new wire verb

1. Spec it in `PROTOCOL.md` — verb name, args, success/error shape, tier requirement.
2. Implement the handler in `agents/windows-modern/src/verbs/<namespace>.cpp` (and the planned NT agent if applicable).
3. Register it in `agents/windows-modern/src/capabilities.cpp` so `system.capabilities` advertises it.
4. Add a test in `tests/conformance/test_<namespace>.py`. Gate it with `needs_verb(capabilities, "<verb>")` so older agents skip rather than fail.
5. *(When `mcp-server/` lands)* Wrap it as a named MCP tool with appropriate `destructiveHint` / `readOnlyHint` annotations.
6. Run the suite: `python tests/conformance/run.py <host> 8765`.

### Adding a new MCP tool *(planned, once `mcp-server/` lands)*

1. Implement the handler in `mcp-server/tools.py`.
2. Gate registration on the wire capability flag — the tool should not be advertised if the agent doesn't speak the verb.
3. Use semantic naming (`click_element` over `click_at_xy`) and lean on `destructiveHint` / `readOnlyHint` to nudge the model.
4. Description copy matters — Claude reads it. Be specific about what the tool does and when it's the right choice over alternatives.

### Running the conformance suite against a Vagrant fixture *(planned)*

Once `examples/vagrant/` lands:

```bash
cd examples/vagrant && vagrant up
python tests/conformance/run.py 127.0.0.1 18765   # vagrant forwards 8765 -> 18765
```

## Things not to do

- **Don't merge `benchmark` into `main`.** It's a fixture branch, not product.
- **Don't `git add` from the repo root without confirming `.gitignore` is intact.** Build artefacts are noisy; staging unintentionally is easy.
- **Don't skip pre-commit hooks** (`--no-verify`). If a hook fails, fix the cause.
- **Don't amend commits** unless the user explicitly asks. Make a new commit instead.
- **Don't add `winget` or store-installed tooling to install scripts** — the agent's own `--install` (and the planned `Tools/install-agent.ps1`) must work on bare VMs without Microsoft Store availability.
- **Don't disable SEH / structured exception handling barriers in the agent** — they catch driver-induced crashes from third-party UI hooks.
- **Don't ship a verb without a conformance test.** The suite is the contract.

## Working with the wire protocol — non-obvious points

- **Capabilities are dynamic.** Don't hardcode the verb list in clients; read `CAPS` from the agent and gate on it. Older agents and the NT build advertise smaller surfaces.
- **UIPI is silent on legacy v1 protocol; explicit on v2.** When the foreground window is at a higher integrity level than the agent, v2 input verbs (`input.click` / `input.type` / `element.invoke` / …) return `ERR uipi_blocked` with `{agent_il, target_il}`. The v1 protocol returned `OK` and dropped the input — the v2 redesign closed that observability hole. When debugging "input ignored" reports, check the agent's integrity vs the foreground window's first.
- **The wire is length-prefixed throughout.** Header lines are line-oriented ASCII; payloads are exactly the byte count stated in the header. `EVENT` frames for `watch.*` subscriptions interleave between command responses on the same connection. `connection.reset` recovers from caller-side wire-desync without dropping the connection.
- **There is no authentication today.** Discovery is opt-in per deployment for that reason; v3.0 introduces SSPI.

## Where design proposals live

Long-form design proposals (RFC-shaped, pre-implementation) live as **GitHub issues** tagged `enhancement` — discussion happens in comments, decisions are captured in the issue body. If a proposal lands and ships, the spec moves into `PROTOCOL.md` (for wire-protocol changes) or the relevant module README.

Don't drop large design docs into this repo unprompted. A `docs/` folder, when needed in future, is for **operational** guidance only (runbooks, gotcha explainers, deployment notes) — not speculative design.

## Future structure decisions (don't pre-empt)

- **v2.0 privsep dispatcher** will need a second binary alongside the agent. Two viable shapes — flat (`agents/windows-modern/dispatcher.cpp` + `worker.cpp` in the same target) or nested (`agents/windows-modern/{dispatcher,worker}/` separate targets). Decide when the work starts; current single-target structure is right for v1.0.
- **`mcp-server/tools.py`** holds all named tools today. If it grows past ~50 tools, split into a `mcp-server/tools/` package by category (`capture.py`, `input.py`, `files.py`, etc.). Don't pre-split.
- **`client/`** holds Python-only references today. If a non-Python client appears (TS, Go), move existing files under `client/python/` and add the new client alongside. Don't pre-create empty language directories.

## Benchmarks and comparisons

Tool-benchmark and comparison sessions are run periodically against the agent (and against alternative remote-control approaches) to surface ergonomic gaps and observability holes. Their raw outputs are not committed to this repo; relevant findings flow into individual GitHub issues tagged `agent-feedback`. When working on such an issue, the issue body is the source of truth.
