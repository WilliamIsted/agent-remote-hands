# Operating Agent Remote Hands as an LLM

If you're an LLM (Claude, GPT, Gemini, anything else) about to use this agent — or about to write code that calls it — start here. This document is the operator's-eye view: what to read, what to assume, what not to.

The agent has a deliberate split between **how it's invoked** and **what's on the wire**. Most LLM clients shouldn't need to think about the wire at all; the MCP bridge handles framing, tier transitions, and tool naming. The wire matters when you're bypassing the bridge — writing a custom test rig, debugging a specific verb's behaviour, or running on a host where the bridge isn't available.

## The 90% path: use the MCP bridge

If you're driving the agent through Claude Code, Claude Desktop, or any MCP-aware client, **you don't need to read `PROTOCOL.md`**. The bridge in `mcp-server/` exposes the wire verbs as named tools (`take_screenshot`, `click_element`, `write_file`, etc.) with a tier-elevation flow that keeps destructive operations behind explicit caller intent.

Read first:
- [`mcp-server/README.md`](mcp-server/README.md) — bridge architecture, tier-elevation flow, environment variables.

Tier elevation is stateful and follows the v2.1 CRUDX ladder (`read` < `create` < `update` < `delete` < `extra_risky`). Call `request_update_access(reason="…")` before tools like `click_element` or `write_file`; call `request_delete_access(reason="…")` before tools like `delete_file` or `kill_process`; call `request_extra_risky_access(reason="…")` before tools like `cancel_pending_shutdown`. (`request_create_access` exists too, for tools that only need create — `directory.create`, `process.start`.) The bridge handles the token dance for you.

## The 10% path: speaking the wire directly

If you're writing a custom client, running on a host without the MCP bridge, or debugging behaviour the bridge layer obscures — read these in order:

1. **[`PROTOCOL.md`](PROTOCOL.md)** — the contract. Framing rules, every verb's argument shape, error codes, the connection state machine, the tier model. This is the source of truth; nothing on the wire works differently from what's documented here.
2. **[`tests/conformance/wire.py`](tests/conformance/wire.py)** — a canonical Python client that implements the framing correctly. ~170 lines, no dependencies beyond the Python stdlib. Use it as a reference, copy it, or just import it. The framing has corners (length-prefixed payloads, EVENT frames interleaving with command responses) that are easy to get wrong from scratch.
3. **[`tests/conformance/test_*.py`](tests/conformance/)** — one file per namespace, with worked examples of every conformant verb call. When you're not sure how to invoke `screen.capture --window` or `registry.read --value`, find the test that does it.

For idiomatic Win32-side reference (e.g., when you need to compare the agent's behaviour against a hand-rolled implementation):

4. **[`tests/conformance/fixtures/`](tests/conformance/fixtures/)** — PowerShell scripts that implement the canonical Win32 form of operations the agent performs. Useful as ground truth when investigating discrepancies. See the README in that directory.

## What the agent tells you at runtime

Two verbs are designed to be your starting point on a fresh connection:

- **`system.info`** — agent identity, OS, hostname, integrity level, monitor count, available image formats, and capability summary. Call it once after `connection.hello`.
- **`system.capabilities`** — the verb→required-tier map, exhaustively. If a verb isn't in the response, this build doesn't implement it. Tiers are CRUDX letters mapped to ladder rungs (R→`read`, C→`create`, U→`update`, D→`delete`, X→`extra_risky`); a verb tagged `tier: extra_risky` needs `connection.tier_raise extra_risky <token>` before calling it.

These two together let you discover what the agent supports without consulting the spec. They're not a substitute for reading `PROTOCOL.md` (the *shape* of arguments isn't advertised), but they're enough to gate-check anything you'd want to call.

## Footguns to know about

The wire protocol is tokenised on whitespace, with **double-quote grouping for args that contain spaces** (PROTOCOL.md §1.2.5, added in v2.1). Wrap any path or arg containing spaces in `"..."`:

- `directory.create "C:\Program Files\demo"` — args=[`C:\Program Files\demo`].
- `directory.rename "src dir" "dst dir"` — two args, each preserving its space.
- Backslashes inside the quotes are literal — Windows paths work without escaping.
- Embedded `"` is not representable on the header line. For raw bytes that contain `"`, use the length-prefixed payload form.
- An unmatched opening `"` returns `ERR invalid_args {message:"unmatched quote in header"}`.

Pre-v2.1 traffic that didn't use quotes still parses identically — quoting is additive, not a rewrite of the format.

The agent rejects unknown `--flag` tokens with `ERR invalid_args { unknown_flag }`. If you misspell a flag — `--cursor` instead of `--no-cursor`, say — you'll get a hard error rather than a silent default. This is intentional: silent flag acceptance was a documented footgun in earlier builds.

`connection.tier_raise` is **not idempotent in the obvious sense**. Raising to a tier you already hold returns `ERR invalid_args` ("use tier_drop for downgrades"). The state machine is strict; query the current tier from `system.info` if you're unsure.

Subscriptions (`watch.*`) interleave `EVENT` frames with command responses on the same connection. If you're using `wire.py`, this is handled — `read_response()` discards events and waits for OK/ERR. If you're rolling your own client, your read loop must distinguish `EVENT <sub-id> <length>\n` from `OK <length>\n` / `ERR <code> <length>\n` and route accordingly.

The agent's `input.click` and `input.scroll` use a coupled `SendInput` pattern with absolute virtual-desktop coordinates — see the canonical-pattern comment block at the top of [`agents/windows-modern/src/verbs/input.cpp`](agents/windows-modern/src/verbs/input.cpp). If you're writing a competing client (e.g., for benchmarking) and your synthetic clicks land at the wrong position, **don't** patch around it with `SetCursorPos` + uncoupled `SendInput`; that's the antipattern this verb exists to prevent.

**Synthetic *keyboard* input is invisible to RawInput / DirectInput / `GetAsyncKeyState` targets.** `input.key`, `input.type`, `input.send_message`, and `input.post_message` all deliver at the user32 message-queue layer. Most Unity 5+ games, many DirectX games, and applications that opt into RawInput poll a kernel-keyboard-state path that user32-layer synthesis doesn't populate. The verbs return `OK` on the wire and have zero in-target effect. Synthetic *mouse* input is unaffected — `input.click` reaches the same targets normally. No out-of-process software workaround exists; if you need keyboard input into one of these targets, you need a kernel-mode driver injector or an in-process companion. See [PROTOCOL.md §4.4](PROTOCOL.md) and [#64](https://github.com/WilliamIsted/agent-remote-hands/issues/64) for the full diagnosis.

**For binary file distribution to the target, use `upload_file` (not `write_file`).** The MCP bridge has a three-tool family for writing files: `write_file` takes a UTF-8 string and is text-only; `write_file_b64` takes a base64-encoded blob and works for small in-context binary content; `upload_file` reads from a path on the controller (where the bridge runs) and pushes to a path on the target (where the agent runs), bytes flowing filesystem-to-filesystem without ever passing through MCP/LLM context. `upload_file` chunks internally for files over 16 MB and is fault-tolerant — retries on transport failure with exponential backoff, halves chunk size on persistent timeouts. Use it for zip/exe/DLL/asset distribution; reserve `write_file` for text content the LLM is generating. See [PROTOCOL.md §4.6](PROTOCOL.md) for the wire-level `file.write` / `file.write_at` primitives the bridge uses.

**`REMOTE_HANDS_HOST=<host>.local` on dual-stack networks can hang the bridge at startup.** Windows mDNS resolution of a `--discoverable` agent's hostname returns every interface address the agent has — typically several IPv6 addresses (link-local, ULA, public-routable across multiple prefixes) ahead of the single IPv4 record. If the controller can't reach those IPv6 addresses (common when IPv4 is bridged between hosts but IPv6 isn't), each TCP connect attempt hangs or fails per dead address before reaching the working IPv4 fallback. ICMP ping passes because it uses different family-selection logic, masking the diagnosis. The MCP bridge's `agent_client.py` prefers IPv4 for `.local` hosts to sidestep this; if you're rolling your own client (PowerShell `TcpClient`, Go `net.Dial`, etc.), do the same — call `getaddrinfo` filtered to `AF_INET` first, fall back to `AF_INET6` only on failure. Or just hard-code the IPv4 literal in `REMOTE_HANDS_HOST` if your network is dual-stack-broken. See [#65](https://github.com/WilliamIsted/agent-remote-hands/issues/65).

## A worked example session

Minimal Python session against an agent on `127.0.0.1:8765`:

```python
import json
from wire import WireClient, OkResponse

with WireClient("127.0.0.1", 8765) as c:
    c.hello("my-llm-rig", "2.1")

    info = c.info()                         # system.info
    caps = c.capabilities()                 # system.capabilities

    # Take a screenshot at the default `read` tier (no token needed).
    r = c.request("screen.capture", "--format", "png")
    assert isinstance(r, OkResponse)
    with open("shot.png", "wb") as f:
        f.write(r.payload)

    # Elevate to update tier for input synthesis. Token from
    # %ProgramData%/AgentRemoteHands/token.
    with open(r"C:\ProgramData\AgentRemoteHands\token") as f:
        token = f.read().strip()
    c.request("connection.tier_raise", "update", token)

    # Now you can drive input.
    c.request("input.click", "100", "100")

    c.request("connection.close")
```

The same flow via the MCP bridge would be a sequence of MCP tool calls — different surface, same semantics — but the wire-level version above is the most reductive form.

## What not to assume

- **Don't assume verb argument shapes from the verb name.** `process.start` doesn't take a structured args object — it takes a single command-line string (currently joined back from whitespace-tokenised positional args). `registry.read` with `--value` takes a path and a value name, not "key + value" together. The shape lives in `PROTOCOL.md`; check there.
- **Don't assume the wire is JSON-framed.** Headers are line-oriented ASCII; payloads are length-prefixed bytes. JSON appears *inside* error details and structured responses, not in framing. Several third-party AI summaries of this project have got this wrong; ignore the binary-length-prefix-around-JSON description that surfaces in some reviews.
- **Don't assume the agent is local.** It might be on a VM, a different physical machine, behind firewall rules. The MCP bridge reads `REMOTE_HANDS_HOST` / `REMOTE_HANDS_PORT` / `REMOTE_HANDS_TOKEN_PATH` for exactly this reason. `system.info` returns the agent's hostname so you can confirm you're talking to the host you think you are.
- **Don't assume input verbs always work.** UIPI (User Interface Privilege Isolation) blocks input from a lower-IL process to a higher-IL window. If `input.click` returns `ERR uipi_blocked` with `{agent_il, target_il}`, the agent isn't broken — the target window is at a higher integrity level than the agent. Document this in your error-handling.

## Filing issues from operational use

If you encounter a bug or ergonomic gap during real use, file it on GitHub with the labels `agent-feedback,agent-authored`. Issues #62 and #63 are examples of LLM-surfaced findings that landed real fixes. Concrete reproduction steps + the agent's response (`OkResponse(payload=...)` or `ErrResponse(code=..., detail=...)`) are the most useful body content; don't include conversation history or LLM-internal reasoning.

## Where this document lives

The release zip ships this file alongside `remote-hands.exe`, `PROTOCOL.md`, `README.md`, and `wire.py`. If you have the binary, you have the spec. If you're browsing the GitHub repo, you found it at the root. Either way, this document is the entry point — read it first.
