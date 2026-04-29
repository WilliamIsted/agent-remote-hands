# Wire protocol

**Version:** 2.0
**Status:** Stable.

The Agent Remote Hands wire protocol is a line-oriented, length-prefixed, request/response protocol over plain TCP. Clients send verbs; agents respond with `OK`, `ERR`, or out-of-band `EVENT` frames for active subscriptions.

This document is the canonical contract. Any agent claiming to speak protocol version 2.0 MUST conform to the framing, error model, and verb semantics defined here. The conformance suite under `tests/conformance/` is the executable contract.

---

## 1. Wire format

### 1.1 Transport

- TCP, default port `8765` (configurable per agent).
- One agent listens on one port; per-thread connection model.
- No transport encryption today. Treat the wire as cleartext on the LAN.

### 1.2 Framing

Every message — request, response, or event — is a single header line followed by an optional length-prefixed payload.

**Header line:**

```
<directive> <args...>\n
```

The header is ASCII, terminated by `\n`. A leading `\r` immediately before `\n` is tolerated and ignored. Tokens within the header are separated by single spaces. The header MUST NOT exceed 65 535 bytes.

**Payload:**

If the directive's grammar specifies a `<length>` argument (always the final argument when present), exactly `<length>` bytes follow the header line. `<length>` is decimal ASCII. A length of `0` means no payload. Payload bytes are opaque to the framing layer — interpretation depends on the verb.

**Examples:**

| Form | Meaning |
|---|---|
| `system.info\n` | Verb with no args, no payload |
| `OK 0\n` | Success, no payload |
| `OK 312\n{...312 bytes of JSON...}` | Success with 312-byte payload |
| `ERR tier_required 47\n{...47 bytes of JSON...}` | Error with structured detail |
| `EVENT sub:7 142\n{...142 bytes...}` | Async event for subscription `sub:7` |
| `file.write /path/foo.txt 1024\n{...1024 bytes...}` | Verb with payload |

**Response framing is uniform.** Every `OK` and `ERR` response carries a length prefix — `0` for empty bodies, otherwise the byte count of the payload that follows. There is no separate "inline-text" shape: verbs that conceptually return scalars wrap them in JSON (e.g. `process.start` → `{"pid":N}`, not `OK N`). The verb tables in §4 list the JSON shape per OK response in the Notes column. Subscriptions emit `EVENT` frames asynchronously between request/response pairs — see §6.

### 1.3 Encoding

- Header bytes are UTF-8.
- Payload bytes are opaque (binary). When a payload is text, it is UTF-8 unless the verb specifies otherwise.
- File paths in verb arguments are UTF-8 and translated to wide-character APIs on Windows targets.

### 1.4 Directives

Three directives appear in responses; one in requests.

| Directive | Direction | Purpose |
|---|---|---|
| `<verb>` | Client → agent | Request |
| `OK` | Agent → client | Successful response to a request |
| `ERR` | Agent → client | Failed response to a request |
| `EVENT` | Agent → client | Asynchronous notification for an active subscription |

`EVENT` frames may interleave between request/response pairs on the same connection — see §6.

---

## 2. Connection lifecycle

### 2.1 State machine

```
TCP connect  ─►  pre-hello  ─►  connected  ─►  closed
                      │              │
                      │              ├─ tier transitions: observe ↔ drive ↔ power
                      │              │
                      └──────────────┴────────►  closed (any time, on socket drop)
```

A new connection starts in **pre-hello** state. Only `connection.hello` and `connection.close` are accepted. Any other verb returns `ERR invalid_state` with detail `{"required":"hello"}`.

After a successful `connection.hello`, the connection is **connected** at tier `observe`. All verbs are accepted, subject to tier requirements.

The connection terminates on `connection.close` (graceful), socket drop, or agent shutdown.

### 2.2 Hello

```
> connection.hello <client-name> <protocol-version>
< OK 0
```

The client identifies itself and asserts a protocol major version. The agent rejects mismatched versions with `ERR protocol_mismatch {"agent":"2","client":"<n>"}`.

`<client-name>` is informational and logged by the agent.

### 2.3 Tier negotiation

Three tiers, ordered: `observe` < `drive` < `power`. Every connection starts at `observe`.

```
> connection.tier_raise <tier> <token>
< OK <length>\n
{"new_tier":"<tier>"}
```

`<token>` is the contents of the agent's token file (see §2.6). The agent verifies the token matches and the requested tier is reachable, then records the new tier on this connection.

`<tier>` MUST be a tier name advertised in `system.info.tiers`. Requests for unknown tiers return `ERR invalid_args`.

```
> connection.tier_drop <tier>
< OK <length>\n
{"new_tier":"<tier>"}
```

Voluntary downgrade. No token required. The target tier MUST be lower than or equal to the current tier; raising via `tier_drop` is `ERR invalid_args`.

### 2.4 Reset

```
> connection.reset
< OK 0
```

Discards any in-flight payload buffer and resets the framing parser. Used to recover from caller-side wire-format errors (a request with the wrong length, an unterminated header, etc.) without dropping the connection.

`connection.reset` does NOT change the tier or cancel subscriptions.

### 2.5 Close

```
> connection.close
< OK 0
(socket closes)
```

Graceful disconnect. The agent drains any pending `EVENT` frames for active subscriptions before sending `OK 0`, then closes the socket.

### 2.6 Token file

Tier elevation requires a token. The agent generates a fresh random token (256 bits, hex-encoded) on each start and writes it to:

```
%ProgramData%\AgentRemoteHands\token
```

ACL'd to the local Administrators group and the agent's run-as account. A caller proves authorisation by reading the file (which requires filesystem access to the agent host) and quoting its contents in `connection.tier_raise`.

On agent restart, the token rotates. Existing connections retain their elevated tier; new elevations require the new token.

### 2.7 Connection limits

The agent advertises `max_connections` in `system.info`. The `(N+1)`th concurrent connection receives:

```
< ERR busy 16\n
{"max":<N>}
```

before the socket is closed by the agent. Clients that need to interleave commands during a subscription open a side connection within the limit.

---

## 3. Capability discovery

### 3.1 `system.info`

The negotiation contract. Returns a JSON object describing the agent's identity, advertised capabilities, current connection state, and supported namespaces.

```
> system.info
< OK <length>\n
{
  "name": "<agent-name>",
  "version": "<agent-build-version>",
  "protocol": "2.0",
  "os": "<os-tag>",
  "arch": "x64",
  "hostname": "<host>",
  "user": "<run-as-account>",
  "integrity": "medium",
  "uiaccess": false,
  "privileges": ["SeShutdownPrivilege"],
  "tiers": ["observe", "drive", "power"],
  "current_tier": "observe",
  "auth": ["token"],
  "max_connections": 4,
  "namespaces": [
    "system", "screen", "window", "input", "element",
    "file", "process", "registry", "clipboard", "watch", "connection"
  ],
  "capabilities": {
    "capture": "wgc",
    "ui_automation": "uia",
    "image_formats": ["png", "webp", "bmp"],
    "discovery": "mdns"
  }
}
```

Field semantics:

| Field | Type | Notes |
|---|---|---|
| `name` | string | Agent product name (e.g. `agent-remote-hands`) |
| `version` | string | Agent build version (e.g. `2.0.0+abc123`) |
| `protocol` | string | Wire protocol version. MUST be `"2.0"` for this spec. |
| `os` | string | Target identifier: `windows-modern`, `windows-nt`, etc. |
| `arch` | string | `x86`, `x64`, `arm64` |
| `hostname` | string | Target machine hostname |
| `user` | string | Run-as account name |
| `integrity` | string \| null | `"low"`, `"medium"`, `"high"`, `"system"`, or `null` if integrity levels don't exist on this OS |
| `uiaccess` | boolean | `true` if the agent process has the UIAccess flag set (lets it drive higher-IL UI) |
| `privileges` | array of strings | Win32 privilege names enabled in the agent's token |
| `tiers` | array of strings | Tiers this agent supports. Always at least `["observe"]`. |
| `current_tier` | string | Tier of the current connection |
| `auth` | array of strings | Supported elevation methods. `"token"` today; `"sspi"` in v3.0. |
| `max_connections` | integer | Concurrent connection cap |
| `namespaces` | array of strings | Verb namespaces this agent advertises |
| `capabilities` | object | Sub-capabilities (capture engine, UIA flavour, image formats, etc.) |

Clients SHOULD check `protocol`, `namespaces`, and `capabilities` before issuing verbs that depend on optional features.

### 3.2 `system.capabilities`

```
> system.capabilities
< OK <length>\n
{
  "system.info": {"tier": "observe"},
  "system.health": {"tier": "observe"},
  "system.reboot": {"tier": "power"},
  "screen.capture": {"tier": "observe"},
  ...
}
```

Map of verb name to required tier (and any verb-specific flags). A verb absent from this map is not implemented by this agent — clients MUST NOT issue it.

---

## 4. Verbs by namespace

Tier shorthand: **O** = observe, **D** = drive, **P** = power, **—** = lifecycle (any tier).

### 4.1 `system.*`

System-level identity, health, and lifecycle operations.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `system.info` | O | — | `OK <len>\n<json>` | See §3.1 |
| `system.capabilities` | O | — | `OK <len>\n<json>` | See §3.2 |
| `system.health` | O | — | `OK 0` | Liveness check |
| `system.shutdown_blockers` | O | — | `OK <len>\n<json>` | Lists windows that called `ShutdownBlockReasonCreate`. JSON: `{"blockers":[{"hwnd":"win:0x...","reason":"..."}]}` |
| `system.lock` | O | — | `OK 0` | `LockWorkStation`. `ERR not_supported` on OS without lock support. |
| `system.reboot` | P | `[--delay <s>] [--force] [--reason <code>]` | `OK <len>\n<json>` | Returns `{"phase":"requested","grace_ms":<n>,"deadline_unix":<n>}`. Connection drops shortly after. |
| `system.shutdown` | P | `[--delay <s>] [--force] [--reason <code>]` | as above | |
| `system.logoff` | P | `[--force]` | as above | |
| `system.hibernate` | P | — | `OK 0` | |
| `system.sleep` | P | — | `OK 0` | |
| `system.power.cancel` | P | — | `OK <len>\n<json>` or `ERR not_found` | Aborts a pending in-process delayed shutdown. JSON: `{"cancelled_until_ms":<n>}`. `ERR not_found {"message":"no pending shutdown"}` if no shutdown is pending. Capability-gated; absent on builds without delayed-shutdown bookkeeping. |

Power verbs MAY return `ERR insufficient_privilege {"missing":"SeShutdownPrivilege"}` if the agent's token lacks the relevant privilege.

**`--delay` semantics (windows-modern).** The agent honours `--delay` by waiting in an in-process timer and then issuing the OS-level shutdown / reboot / logoff. Four caller-visible consequences follow:

- **Eager `OK`.** The response is sent before the timer fires. `OK` confirms that the request was accepted and the timer was scheduled — not that the OS-level call later succeeded. Failures of the deferred call are written to the agent log but are not surfaced to the wire.
- **Timer is agent-bound.** If the agent process exits (crash, restart, separate `system.shutdown` without `--delay`, external `taskkill`) before the timer fires, the scheduled action does not happen.
- **One pending request at a time.** A second `--delay`-bearing power verb while one is already pending returns `ERR conflict {"pending_until_ms":<n>}`. Cancel the existing one (`system.power.cancel`) before scheduling another, or omit `--delay` to fire immediately.
- **Cancellable.** Use `system.power.cancel` to abort a pending delayed shutdown; the detached timer thread wakes early and the OS-level call is skipped. The cancel verb is capability-gated — clients should consult `system.capabilities` before calling.

`--delay 0` (the default) bypasses the timer entirely and reports failure synchronously via `ERR not_supported {"win32_error":<n>}`.

### 4.2 `screen.*`

Pixel capture.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `screen.capture` | O | `[--region <x>,<y>,<w>,<h>] [--window <hwnd>] [--format <fmt>]` | `OK <len>\n<bytes>` | `<fmt>` ∈ `png` (default), `webp`, `webp:<quality>`, `bmp`. Format availability is advertised in `system.info.capabilities.image_formats`. |

If neither `--region` nor `--window` is supplied, the entire virtual screen is captured.

**Format guidance.** PNG is lossless and the right default for one-shot stills, but compresses poorly on the kind of large-flat-region content that dominates Windows UI screenshots (~1.5 MB for a 1080p flat dark UI is typical). For bandwidth-sensitive use — `watch.region` streams or polling-style `screen.capture` loops — prefer `webp:70` (or `webp:50` for lower-fidelity acceptable cases). Reserve PNG for cases where pixel-exact comparison or downstream diffing matters.

### 4.3 `window.*`

Top-level window enumeration and control. All `<hwnd>` values use the prefix `win:` followed by the hex window handle (e.g. `win:0x1A2B`).

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `window.list` | O | `[--filter <pattern>] [--all]` | `OK <len>\n<json>` | Visible top-level windows by default; `--all` includes invisible. JSON: `{"windows":[{"hwnd":"win:...","x":N,"y":N,"w":N,"h":N,"title":"...","pid":N}]}` |
| `window.find` | O | `<title-pattern>` | `OK <len>\n<json>` or `ERR not_found` | Returns first match |
| `window.focus` | D | `<hwnd>` | `OK <len>\n<json>` | JSON: `{"prior_hwnd":"win:..."}`. `ERR lock_held` if foreground-lock denied. |
| `window.close` | D | `<hwnd>` | `OK 0` | `WM_CLOSE`. Window may decline. |
| `window.move` | D | `<hwnd> <x> <y> <w> <h>` | `OK 0` | |
| `window.state` | O | `<hwnd>` | `OK <len>\n<json>` | JSON: `{"state":"<state>"}` where state ∈ `minimised`, `maximised`, `normal`, `hidden` |

### 4.4 `input.*`

Synthetic input. All input verbs check the foreground window's integrity level before injecting; if the target IL exceeds the agent's IL, the verb returns `ERR uipi_blocked`.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `input.click` | D | `<x> <y> [--button left\|right\|middle]` | `OK 0` | Default button: left |
| `input.move` | D | `<x> <y>` | `OK 0` | Cursor move; no click |
| `input.scroll` | D | `<x> <y> <delta>` | `OK 0` | Delta in wheel notches (positive = up) |
| `input.key` | D | `<vk> [--modifiers <list>]` | `OK 0` | `<vk>` is a key name (`enter`, `F4`, `a`, …); modifiers comma-separated (`ctrl,shift`) |
| `input.type` | D | `<length>` | `OK 0` | UTF-8 text payload follows the header. Handles Unicode and quote-escape hazards. |
| `input.send_message` | D | `<hwnd> <msg> <wparam> <lparam>` | `OK 0` | Low-level escape hatch for `SendMessage` (synchronous) |
| `input.post_message` | D | `<hwnd> <msg> <wparam> <lparam>` | `OK 0` | Non-blocking peer of `input.send_message`; uses `PostMessage`. Use when the target's message pump is unresponsive to a synchronous send. |

### 4.5 `element.*`

UI Automation. Element identifiers use the prefix `elt:` followed by a connection-scoped sequential integer (e.g. `elt:7`). IDs start at 1 on each new connection and never persist across reconnects.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `element.list` | O | `[--region <x>,<y>,<w>,<h>]` | `OK <len>\n<json>` | Filtered enumeration of interactable / named elements |
| `element.tree` | O | `<elt-id>` | `OK <len>\n<json>` | TreeWalker recursive descent. JSON: `{"elements":[{"depth":N,"id":"elt:N","role":"...","name":"...","bounds":[x,y,w,h],"flags":[...]}]}` |
| `element.at` | O | `<x> <y>` | `OK <len>\n<json>` or `ERR not_found` | Hit test |
| `element.find` | O | `<role> <name-pattern>` | `OK <len>\n<json>` or `ERR not_found` or `ERR uia_blind` | `not_found` = nothing matched. `uia_blind` = UIA cannot see across the integrity barrier (caller may need to elevate). |
| `element.wait` | O | `<role> <name-pattern> <timeout-ms>` | `OK <len>\n<json>` or `ERR timeout` | Polling form of `element.find` (re-walks the visible-element subtree every 250 ms until match or deadline). Returned id is valid for `element.invoke` on the same connection. Capability-gated. |
| `element.find_invoke` | D | `<role> <name-pattern>` | `OK 0` or `ERR not_found` / `ERR uia_blind` / `ERR not_supported_by_target` / `ERR target_gone` | Compound verb: `element.find` + `element.invoke` in one round-trip. Same matching rules as `element.find`. |
| `element.at_invoke` | D | `<x> <y>` | `OK 0` or `ERR not_found` / `ERR not_supported_by_target` / `ERR target_gone` | Compound verb: `element.at` + `element.invoke` in one round-trip. |
| `element.invoke` | D | `<elt-id>` | `OK 0` or `ERR not_supported_by_target` or `ERR target_gone` | InvokePattern |
| `element.toggle` | D | `<elt-id>` | `OK <len>\n<json>` | TogglePattern. JSON: `{"new_state":"<state>"}` ∈ `on`, `off`, `indeterminate` |
| `element.expand` | D | `<elt-id>` | `OK <len>\n<json>` | ExpandCollapsePattern.Expand. JSON: `{"new_state":"<state>"}` |
| `element.collapse` | D | `<elt-id>` | `OK <len>\n<json>` | ExpandCollapsePattern.Collapse |
| `element.focus` | D | `<elt-id>` | `OK 0` | Sets keyboard focus |
| `element.text` | O | `<elt-id>` | `OK <len>\n<bytes>` | TextPattern preferred; ValuePattern fallback. Empty payload (`OK 0`) is valid. |
| `element.set_text` | D | `<elt-id> <length>` | `OK 0` or `ERR readonly` or `ERR not_supported_by_target` | UTF-8 text payload follows |

Element IDs are allocated by the agent on calls that produce element references (`element.list`, `element.tree`, `element.at`, `element.find`, `element.wait`). IDs remain valid for the connection's lifetime unless the underlying element is invalidated, in which case subsequent verbs return `ERR target_gone`.

### 4.6 `file.*`

Filesystem operations. Paths are UTF-8.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `file.read` | O | `<path>` | `OK <len>\n<bytes>` | Whole-file read |
| `file.write` | D | `<path> <length>` | `OK 0` | Whole-file write; payload follows |
| `file.list` | O | `<path>` | `OK <len>\n<json>` | JSON: `{"entries":[{"name":"...","type":"file\|dir\|link","size":N,"mtime_unix":N}]}` |
| `file.stat` | O | `<path>` | `OK <len>\n<json>` | Single-entry version of `file.list` |
| `file.delete` | P | `<path>` | `OK 0` | Files and empty directories |
| `file.exists` | O | `<path>` | `OK <len>\n<json>` | JSON: `{"exists":true,"type":"..."}` or `{"exists":false}` |
| `file.wait` | O | `<pattern> <timeout-ms>` | `OK <len>\n<json>` or `ERR timeout` | Resolves on path matching glob |
| `file.mkdir` | D | `<path>` | `OK 0` | Single directory level; parent must exist |
| `file.rename` | D | `<src> <dst>` | `OK 0` | Move or rename |

### 4.7 `process.*`

Process management.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `process.list` | O | `[--filter <pattern>]` | `OK <len>\n<json>` | JSON: `{"processes":[{"pid":N,"image":"...","ppid":N}]}` |
| `process.start` | D | `<argv> [--stdin <length>]` | `OK <len>\n<json>` | `CreateProcess`. Returns `{"pid":N}`. Optional length-prefixed stdin payload. |
| `process.shell` | D | `<path> [--args <s>] [--verb <v>]` | `OK <len>\n<json>` | `ShellExecuteEx`. Handles paths with spaces / unicode without shell-escape hazards. `--args` is the parameter string passed verbatim to the spawned program; `--verb` selects a non-default verb (e.g. `runas` for elevation, `print`, `edit`). Returns `{"pid":N}` (PID may be 0 for verbs that don't spawn a process). |
| `process.kill` | P | `<pid>` | `OK 0` | `TerminateProcess` |
| `process.wait` | O | `<pid> <timeout-ms>` | `OK <len>\n<json>` or `ERR timeout` | JSON: `{"exit_code":N}`. Returns successfully even if the process has already exited. |

### 4.8 `registry.*`

Windows registry. Paths use the standard `HKLM\Software\...` form (or `HKCU`, `HKCR`, `HKU`, `HKCC`).

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `registry.read` | O | `<path> [--value <name>]` | `OK <len>\n<json>` | JSON: `{"type":"REG_SZ","data":"..."}` for single value; `{"values":{...},"subkeys":[...]}` for whole key |
| `registry.write` | D | `<path> <name> <type> <data>` | `OK 0` | `<type>` ∈ standard `REG_*` names |
| `registry.delete` | P | `<path> [--value <name>]` | `OK 0` | Whole key if `--value` absent |
| `registry.wait` | O | `<path> <timeout-ms>` | `OK <len>\n<json>` or `ERR timeout` | `RegNotifyChangeKeyValue`-based |

### 4.9 `clipboard.*`

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `clipboard.read` | O | — | `OK <len>\n<bytes>` | UTF-8 text. Empty payload if clipboard has no text. |
| `clipboard.write` | D | `<length>` | `OK 0` | UTF-8 text payload |

### 4.10 `watch.*`

Subscription-based observation. See §6 for the EVENT-frame mechanics.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `watch.region` | O | `<x>,<y>,<w>,<h> [--interval <ms>] [--until-change]` | `OK <len>\n<json>` | JSON: `{"subscription_id":"sub:N"}`. Emits `EVENT sub:N <bytes>` per frame (image bytes, format from `system.info.capabilities.image_formats`). |
| `watch.process` | O | `<pid>` | as above | Emits one `EVENT sub:N <json>` on process exit, then auto-cancels |
| `watch.window` | O | `--title-prefix <pattern>` | as above | Emits `EVENT` on window appear / disappear |
| `watch.element` | O | `<elt-id>` | as above | Emits `EVENT` on element invalidation, then auto-cancels |
| `watch.file` | O | `<pattern>` | as above | `ReadDirectoryChangesW`-based |
| `watch.registry` | O | `<path>` | as above | `RegNotifyChangeKeyValue`-based |
| `watch.cancel` | O | `<sub-id>` | `OK 0` | Ends a subscription. Idempotent on already-cancelled IDs. |

### 4.11 `connection.*`

Lifecycle and tier negotiation. See §2 for state machine details.

| Verb | Tier | Args | Response | Notes |
|---|---|---|---|---|
| `connection.hello` | — | `<client-name> <protocol-version>` | `OK 0` | First verb after connect |
| `connection.tier_raise` | — | `<tier> <token>` | `OK <len>\n<json>` | Token gates the raise |
| `connection.tier_drop` | — | `<tier>` | `OK <len>\n<json>` | Voluntary downgrade; no token |
| `connection.reset` | — | — | `OK 0` | Flush wire-format state |
| `connection.close` | — | — | `OK 0`, then close | Drains pending EVENT frames before close |

---

## 5. Error codes

Errors take the form:

```
< ERR <code> [<length>\n<json-detail>]
```

`<code>` is a stable identifier suitable for client-side switching. `<json-detail>`, when present, carries diagnostic context.

### 5.1 Verb-agnostic codes

| Code | Detail fields | Meaning |
|---|---|---|
| `tier_required` | `{required, current}` | Current tier insufficient for this verb |
| `not_supported` | `{verb, reason?}` | Verb / capability not advertised on this agent |
| `invalid_args` | `{message}` | Malformed verb call |
| `invalid_state` | `{required}` | Verb not valid in current connection state (e.g. pre-hello) |
| `wire_desync` | — | Caller payload corrupt; recoverable via `connection.reset` |
| `timeout` | `{deadline}` | `*.wait` or streaming verb expired |
| `busy` | `{max}` | Too many concurrent connections |
| `conflict` | varies | Verb cannot proceed because of in-flight state (e.g. `system.power` `--delay` while one is already pending — detail `{"pending_until_ms":<n>}`) |
| `protocol_mismatch` | `{agent, client}` | Hello specified an incompatible protocol version |
| `auth_required` | — | Reserved for v3.0 SSPI |
| `auth_invalid` | — | Reserved for v3.0 SSPI |

### 5.2 Domain-specific codes

| Code | Detail fields | Meaning |
|---|---|---|
| `target_gone` | `{handle, last_known_state?}` | Element / window / process vanished mid-call |
| `uipi_blocked` | `{agent_il, target_il}` | Integrity barrier blocked input |
| `not_found` | — | Search returned nothing |
| `uia_blind` | — | UIA cannot see across the IL barrier; distinct from `not_found` |
| `lock_held` | `{lock_type, holder?}` | Foreground / clipboard / registry lock denied |
| `readonly` | — | Write to immutable target |
| `not_supported_by_target` | `{pattern?}` | Target element / object lacks the required UIA pattern |
| `insufficient_privilege` | `{missing}` | Token lacks the required Win32 privilege |

### 5.3 Detail JSON

When present, detail is UTF-8 JSON. Agents SHOULD always include detail for codes that benefit from diagnostic context (`tier_required`, `uipi_blocked`, `target_gone`, `timeout`, `lock_held`). Detail MAY include a `message` field with a human-readable explanation; clients SHOULD treat the code as the authoritative signal and the message as informational.

---

## 6. Subscriptions and EVENT frames

Long-running observation operations register subscriptions on the connection. Each `watch.*` verb returns immediately with `OK <len>\n{"subscription_id":"sub:N"}`. The subscription is active until the client issues `watch.cancel` or the connection closes.

### 6.1 EVENT framing

Events arrive interleaved with regular request/response pairs:

```
< EVENT <subscription_id> <length>\n
<bytes>
```

Subscription ID format: `sub:` followed by a connection-scoped sequential integer.

EVENT frames are atomic: they appear between request boundaries, never mid-response. A client reading the wire MUST be prepared to receive EVENT frames at any point after the first `OK` returned from a `watch.*` verb.

### 6.2 EVENT payload shape

Payload format depends on the watch type:

| Watch verb | EVENT payload |
|---|---|
| `watch.region` | Image bytes (PNG / WebP / BMP per the `--format` argument or the agent's default) |
| `watch.process` | UTF-8 JSON: `{"kind":"process_exit","pid":N,"exit_code":N}` |
| `watch.window` | UTF-8 JSON: `{"kind":"window_appeared\|window_gone","hwnd":"win:...","title":"..."}` |
| `watch.element` | UTF-8 JSON: `{"kind":"element_invalidated","elt":"elt:N"}` |
| `watch.file` | UTF-8 JSON: `{"kind":"created\|modified\|deleted","path":"..."}` |
| `watch.registry` | UTF-8 JSON: `{"kind":"changed","path":"..."}` |

### 6.3 Auto-cancellation

Some subscriptions end on their own:

- `watch.process` auto-cancels after emitting the `process_exit` event.
- `watch.element` auto-cancels after emitting the `element_invalidated` event.
- `watch.region --until-change` auto-cancels after emitting one frame.

For auto-cancelled subscriptions, the client MAY issue `watch.cancel` defensively; the agent returns `OK 0` whether or not the subscription is still active.

### 6.4 Ordering

Within a single subscription, EVENT frames are emitted in the order events occurred. Across subscriptions, frame ordering is unspecified.

---

## 7. Tier model

### 7.1 Tier semantics

| Tier | Capability |
|---|---|
| `observe` | Read-only operations. Cannot synthesise input, cannot modify files / registry, cannot terminate processes, cannot reboot. |
| `drive` | All `observe` capabilities plus synthetic input, file writes, registry writes, process start, focus changes. |
| `power` | All `drive` capabilities plus destructive operations: process kill, file delete, registry delete, system reboot / shutdown / logoff. |

### 7.2 Tier enforcement

The agent MUST enforce tier requirements at the verb-dispatch layer. A verb whose required tier exceeds the connection's current tier returns:

```
< ERR tier_required <length>\n
{"required":"<tier>","current":"<tier>"}
```

### 7.3 Privilege management

The agent MAY enable Win32 privileges (e.g. `SeShutdownPrivilege`) only when needed for the current tier. An observe-tier connection SHOULD see those privileges disabled in the agent's effective token.

This is OS-enforced confinement layered on top of protocol-level tier checks. v2.0 agents are single-process; full process isolation is the v2.0+ privsep dispatcher milestone.

---

## 8. Elevation and integrity levels

Windows runs each process at one of four mandatory integrity levels — `low`, `medium`, `high`, `system`. User Interface Privilege Isolation (UIPI) silently blocks synthetic input from a lower-IL process to a higher-IL window. This is the agent's most common silent-failure mode and is worth understanding before deploying.

The OS-level integrity model is **separate** from the wire-protocol tier model in §7: tiers gate wire verbs (observe / drive / power) and live entirely in the agent process; integrity levels gate cross-process effects and are enforced by the kernel. A connection at the `power` tier whose agent runs at `medium` IL still cannot drive a `high`-IL installer wizard.

### 8.1 The Medium-IL agent / High-IL installer trap

When started by a Task Scheduler logon-task with default settings, the agent runs at **Medium IL** in the logged-on user's session. Common installers — Mozilla NSIS, MSI installs, Steam, anything carrying a `requestedExecutionLevel="requireAdministrator"` manifest — auto-elevate to **High IL**.

UIPI then drops every synthesised input that crosses the IL boundary upward. From the wire, every diagnostic looks fine: `element.find` returns `OK <id>`, `element.invoke` returns `OK`, `input.key` returns `OK` — but the wizard never advances. v1 agents had no observability into this; v2 agents surface it explicitly (see §8.3).

### 8.2 Surfacing the agent's IL

`system.info` (§3.1) returns two fields:

| Field | Values | Meaning |
|---|---|---|
| `integrity` | `low` / `medium` / `high` / `system`, or `null` on platforms without IL | The agent's own IL |
| `uiaccess` | boolean | Whether the agent's binary is signed with `uiAccess="true"` — exempts it from UIPI for synthetic input |

A caller that sees `integrity=medium` and `uiaccess=false` knows up front that cross-IL automation against any high-IL installer will fail.

### 8.3 Runtime UIPI failure surfaces

| Error | Detail | Returned by | Meaning |
|---|---|---|---|
| `uipi_blocked` | `{agent_il, target_il}` | `input.*`, `element.invoke`, `element.find_invoke`, `element.at_invoke` | Synthetic input rejected because the target window's IL exceeds the agent's |
| `uia_blind` | `{agent_il, target_il}` | `element.find`, `element.find_invoke`, `element.wait` | UIA tree walk completed without a match, *and* a higher-IL foreground window is present — distinguishes "element doesn't exist" from "I can't see across the barrier" |

Both are deterministic: callers can branch on the code rather than retrying blindly.

### 8.4 Workarounds for cross-IL automation

1. **Spawn a second, elevated agent.** Run a second `remote-hands.exe --port 8766` under an elevated token. The Medium-IL agent handles ordinary automation; the elevated one drives installer wizards. Callers pick the agent that matches the target window's IL.
2. **Sign the agent with `uiAccess="true"`.** Embedding `<requestedExecutionLevel uiAccess="true" level="asInvoker"/>` in the manifest, signing with a trusted code-signing certificate, and installing the binary under `Program Files` exempts the agent from UIPI without making it elevated. This is the path accessibility tools use; once `system.info.uiaccess=true`, cross-IL input verbs work as if the agent were High-IL.
3. **`--install` with the registering user already in `BUILTIN\Administrators`.** The installed Task Scheduler task uses `HighestAvailable`; if the user is an admin, the task runs elevated on their next logon, making the agent itself High-IL.

(1) is simplest; (2) is most ergonomic for production deployments; (3) is the fastest path on dev boxes where the user is already a local admin.

### 8.5 Related sections

- `system.info` field shapes — §3.1
- Error codes `uipi_blocked` and `uia_blind` — §5.2
- Wire-protocol tier model — §7

---

## 9. Discovery

Agents MAY advertise themselves on the local network via mDNS / DNS-SD when started with `REMOTE_HANDS_DISCOVERABLE=1` or `--discoverable`.

Service type: `_remote-hands._tcp.local.`

TXT record fields:

| Field | Example | Meaning |
|---|---|---|
| `protocol` | `2` | Wire protocol major version |
| `os` | `windows-modern` | Target identifier |
| `tiers` | `observe,drive,power` | Comma-separated tier list |
| `auth` | `token` | Comma-separated auth methods |

Discovery is opt-in per deployment. The protocol has no transport authentication, so mDNS advertising on an untrusted network is a footgun.

---

## 10. Behaviour notes

### 10.1 Concurrency

Verbs from a single connection are serialised — the agent processes them in receive order and responses arrive in the same order. Verbs across different connections may run concurrently.

### 10.2 Idempotency

Most verbs are not idempotent. Callers that require at-most-once semantics SHOULD wrap retry logic with appropriate guards.

`watch.cancel` is idempotent: cancelling an already-cancelled subscription returns `OK 0`.

### 10.3 Element id stability

Element IDs are stable for the connection lifetime unless the underlying UI element is destroyed or invalidated. After invalidation, calls referencing the ID return `ERR target_gone`. The ID is not reused for a different element on the same connection.

### 10.4 Foreground locks

Windows enforces foreground-window locks to prevent applications from stealing focus. When `window.focus` is denied by this mechanism, the agent returns `ERR lock_held` rather than silently succeeding. Callers may retry after granting their own process the foreground privilege via `AllowSetForegroundWindow`.

### 10.5 UIPI behaviour

See §8 for the full discussion of integrity-level interactions. Briefly: synthetic input verbs (`input.*`, `element.invoke`) and UIA-based search verbs (`element.find`, `element.wait`) surface cross-IL barriers explicitly via `ERR uipi_blocked` and `ERR uia_blind` rather than silently succeeding.

### 10.6 Wire-desync recovery

If a client sends a malformed request (mis-stated payload length, header exceeding 65 535 bytes, etc.), the agent SHOULD return `ERR wire_desync` and discard the inbound buffer. The client recovers by sending `connection.reset` and resuming.

---

## 11. Worked examples

### 11.1 Minimal session

```
> connection.hello agent-remote-hands 2.0
< OK 0
> system.info
< OK 312
{"name":"win-host-42","protocol":"2.0","os":"windows-modern","integrity":"medium",
 "tiers":["observe","drive","power"],"current_tier":"observe", ...}
> system.health
< OK 0
> connection.close
< OK 0
```

### 11.2 Tier elevation and input

```
> input.click 100 200
< ERR tier_required 38
{"required":"drive","current":"observe"}

> file.read C:\ProgramData\AgentRemoteHands\token
< OK 64
a3f1c8...e9b2

> connection.tier_raise drive a3f1c8...e9b2
< OK 22
{"new_tier":"drive"}

> input.click 100 200
< OK 0
```

### 11.3 UIPI surfacing

```
> input.type 11
hello world

< ERR uipi_blocked 47
{"agent_il":"medium","target_il":"high","message":"foreground window owned by elevated process"}
```

### 11.4 Subscription with interleaved verbs

```
> watch.window --title-prefix "Mozilla Firefox"
< OK 25
{"subscription_id":"sub:7"}

> screen.capture
< OK 184321
<png bytes>

< EVENT sub:7 142
{"kind":"window_appeared","hwnd":"win:0x1A2B","title":"Mozilla Firefox - Mozilla Firefox","pid":4321}

> window.focus win:0x1A2B
< OK 22
{"prior_hwnd":"win:0x0844"}

> watch.cancel sub:7
< OK 0
```

### 11.5 Reboot

```
> connection.tier_raise power <token>
< OK 22
{"new_tier":"power"}

> system.reboot --delay 5 --reason planned
< OK 64
{"phase":"requested","grace_ms":5000,"deadline_unix":1748438201}

(connection drops within ~6 seconds)
```

### 11.6 Wire desync recovery

```
> file.write /path/foo.txt 1024
<sends only 800 bytes, then sends another verb header>
< ERR wire_desync 0
> connection.reset
< OK 0
> file.write /path/foo.txt 1024
<sends 1024 bytes correctly>
< OK 0
```

---

## 12. Versioning policy

### 12.1 Major version

The protocol major version (currently `2`) appears in:

- `system.info.protocol`
- mDNS TXT record `protocol=`
- The `connection.hello` second argument

Major version changes when the wire format, framing rules, or core semantics break compatibility. Clients MUST refuse to operate against an agent whose major version differs from the version they were built against.

### 12.2 Minor version

Minor versions add verbs, capability flags, or error codes without breaking existing clients. Clients SHOULD ignore unrecognised fields in `system.info`, unrecognised verbs in `system.capabilities`, and unrecognised error codes (treating them as opaque strings).

### 12.3 Conformance

An agent claims conformance to a protocol version by:

1. Returning that version string from `system.info.protocol`.
2. Implementing every verb advertised in `system.capabilities` per this spec.
3. Passing the conformance suite for that version.

The conformance suite under `tests/conformance/` is the executable contract.

### 12.4 Capability advertisement

Verbs not implemented on a particular target MUST be omitted from `system.capabilities`. Clients MUST NOT issue verbs absent from the capabilities map; agents MAY return `ERR not_supported` if they do.

---

## Appendix A: Verb summary

A flat list of all v2.0 verbs for quick reference. **O/D/P** denotes the required tier; **—** denotes lifecycle (any tier).

```
connection.hello             (—)
connection.tier_raise        (—)
connection.tier_drop         (—)
connection.reset             (—)
connection.close             (—)

system.info                  (O)
system.capabilities          (O)
system.health                (O)
system.shutdown_blockers     (O)
system.lock                  (O)
system.reboot                (P)
system.shutdown              (P)
system.logoff                (P)
system.hibernate             (P)
system.sleep                 (P)
system.power.cancel          (P)

screen.capture               (O)

window.list                  (O)
window.find                  (O)
window.focus                 (D)
window.close                 (D)
window.move                  (D)
window.state                 (O)

input.click                  (D)
input.move                   (D)
input.scroll                 (D)
input.key                    (D)
input.type                   (D)
input.send_message           (D)
input.post_message           (D)

element.list                 (O)
element.tree                 (O)
element.at                   (O)
element.find                 (O)
element.wait                 (O)
element.find_invoke          (D)
element.at_invoke            (D)
element.invoke               (D)
element.toggle               (D)
element.expand               (D)
element.collapse             (D)
element.focus                (D)
element.text                 (O)
element.set_text             (D)

file.read                    (O)
file.write                   (D)
file.list                    (O)
file.stat                    (O)
file.delete                  (P)
file.exists                  (O)
file.wait                    (O)
file.mkdir                   (D)
file.rename                  (D)

process.list                 (O)
process.start                (D)
process.shell                (D)
process.kill                 (P)
process.wait                 (O)

registry.read                (O)
registry.write               (D)
registry.delete              (P)
registry.wait                (O)

clipboard.read               (O)
clipboard.write              (D)

watch.region                 (O)
watch.process                (O)
watch.window                 (O)
watch.element                (O)
watch.file                   (O)
watch.registry               (O)
watch.cancel                 (O)
```

55 verbs across 11 namespaces.
