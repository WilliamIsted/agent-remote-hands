# To Do

Future work for Agent Remote Hands. Items here are flagged for consideration; nothing is committed yet.

## Install / provisioning — windows-modern landed; backfill remaining

- ✅ **`Tools/install-agent.ps1`** — windows-modern installer. Places binary, opens firewall, registers Task Scheduler logon task, sets opt-in env vars (`REMOTE_HANDS_DISCOVERABLE`, `REMOTE_HANDS_POWER`), starts the agent.
- ✅ **`examples/vagrant/Vagrantfile`** — end-to-end provisioning fixture for Win11.

Remaining:

- **`Tools/install-agent.cmd`** — windows-nt installer. PowerShell isn't reliably present on XP/2003; use plain `.cmd` with `bitsadmin` (XP+) or `certutil -urlcache` (2003+) for downloads. Same flow: place binary, open firewall via `netsh advfirewall`, set env vars via `setx`, register service via the binary's `--install`. ~80 lines.
- **GitHub Actions release pipeline** — automate the build for both targets and attach the binaries to a tag-triggered release. Lets the install scripts default to `-Url https://github.com/.../releases/latest/download/remote-hands-modern.exe` instead of requiring a local build. ~30 lines of YAML.
- **More provisioner examples** under `examples/`:
  - `examples/packer/` — Packer template that bakes the agent into a reusable Win11 image
  - `examples/cloud-init/` — user-data scripts for AWS EC2, Azure VM, GCP Compute Engine
  - `examples/hyperv-quickcreate/` — for Win11 Hyper-V users on Windows hosts
  - `examples/wsl/` — agents inside WSL2 distros (Linux target, separate from windows-* agents)
- **Code signing** for the release binaries — eliminates SmartScreen warnings on fresh installs. Requires a $200/yr cert; defer until shipping publicly.
- **MCP-driven install** — a tool in `mcp-server/` that drives an installer on a remote VM via hypervisor guest control APIs (VBoxManage / vmrun / Invoke-Command over PSDirect / prlctl). Stretch goal; only worth doing once a concrete user demands it.

## Microsoft UI Automation (UIA) / MSAA

Pixel-based navigation is fragile (DPI / theming / layout / locale all shift the image). UIA exposes the accessibility tree — controls by semantic name and role — so the AI can locate "the OK button" in microseconds, robust to those shifts. The pixel-based capture/input verbs remain the fallback for surfaces UIA can't see (canvases, in-browser DOM, custom-drawn controls, games).

### MVP — landed

A first slice of UIA is implemented on `agents/windows-modern/` (see `uia.h` / `uia.cpp`):

- ✅ `ELEMENTS [<x> <y> <w> <h>]` — enumerate filtered tree (interactable / named, on-screen)
- ✅ `ELEMENT_AT <x> <y>` — hit test
- ✅ `ELEMENT_FIND <role> <name-substr>` — semantic search
- ✅ `ELEMENT_INVOKE <id>` — invoke pattern (no synthesised click)
- ✅ `ELEMENT_FOCUS <id>` — set keyboard focus
- ✅ `ui_automation=uia` advertised in INFO
- ✅ Per-connection element id space (regenerative on each `ELEMENTS`)

### Remaining UIA verbs

To round out the pattern coverage on `agents/windows-modern/`:

| Verb | Pattern | Purpose |
|------|---------|---------|
| `ELEMENT_TREE <id>` | walker | Recursive descent under an id; rows include leading `<depth>` so the client can reconstruct the tree. Useful for "what's inside this dialog/menu/list?" |
| `ELEMENT_TEXT <id>` | TextPattern / ValuePattern | Read full text content (rich edits, documents) — distinct from the `<value>` field in row format which only carries ValuePattern's terse form. |
| `ELEMENT_SET_TEXT <id> <length>\n<bytes>` | ValuePattern | Write into edit fields via the API rather than synthesised KEYS. |
| `ELEMENT_TOGGLE <id>` | TogglePattern | Checkboxes, toggle buttons. |
| `ELEMENT_EXPAND <id>` / `ELEMENT_COLLAPSE <id>` | ExpandCollapsePattern | Combo boxes, tree nodes, expandable menu items. |

Effort: ~200 lines per pattern roughly; the COM scaffolding is in place from the MVP, so each new verb is a focused handler + pattern interface QI.

### MSAA on legacy targets

- **`agents/windows-nt/`** — `IAccessible` from `oleacc.dll`. Available NT4+. More limited model: no pattern interfaces — one `accDoDefaultAction` subsumes Invoke/Toggle/Expand. Verb set reduces to `ELEMENTS`, `ELEMENT_AT`, `ELEMENT_TREE`, `ELEMENT_DEFAULT_ACTION`, `ELEMENT_FOCUS`, `ELEMENT_VALUE`, `ELEMENT_SET_VALUE`, `ELEMENT_FIND`. INFO advertises `ui_automation=msaa`. ~1 day of work.
- **`agents/windows-9x/`** — Active Accessibility 2.0 redist (`oleacc.dll`) on 95/98/ME. Same scope as windows-nt MSAA work; redist needs to ship alongside the agent or the user installs it once.

## App-crash detection — interrupt the agent when the target app dies

**Problem:** an AI client tells the agent "open Settings, click X, type Y." If Settings.exe crashes mid-sequence (or the OS killed it, or it was never running), the agent happily synthesises clicks and keystrokes against whatever's underneath — wasting tokens, time, and producing nonsense actions. The model sees screenshots that don't match its mental state and tries to recover, often making things worse.

**Goal:** when the app the AI is interacting with disappears, the agent notices and reports back so the model can stop and re-orient instead of churning.

### Detection signals (in order of confidence)

1. **Tracked-PID death** — most reliable. When the AI launches a process via `EXEC` / `RUN`, or when it operates on a UIA element whose backing process the agent can identify (`IUIAutomationElement::get_CurrentProcessId`), the agent records the PID. A periodic `WaitForSingleObject(0)` poll detects exit immediately. **No false positives.**
2. **Foreground-window owner change** — the AI brings a window to the front via `WINFOCUS` or `click_element`. Subsequently the foreground window's owning process changes (or `GetForegroundWindow` returns NULL) → the targeted app is gone or backgrounded. False positives possible if the user manually alt-tabs, but rare in unattended automation.
3. **Window-handle invalidation** — `IsWindow(hwnd)` returns false. Cheap, definitive for that specific window.
4. **UIA element handle staleness** — calling a method on a cached `IUIAutomationElement*` returns `UIA_E_ELEMENTNOTAVAILABLE`. Tells us the element specifically is gone; doesn't always mean the whole app crashed (could just be a closed dialog).
5. **Crash-dialog detection** — Windows Error Reporting throws up a "Program X has stopped working" dialog. Detect by enumerating top-level windows and matching a crash-dialog signature. Less reliable; varies by Windows version, locale, and WER configuration.

### Proposed protocol surface

A new optional verb that lets the AI client **subscribe** to crash events for a target. Subscriptions live for the connection's lifetime and fire as soon as the agent detects the death.

```
WATCH_PROCESS <pid>
    OK <subscription_id>
WATCH_WINDOW <title-prefix>
    OK <subscription_id>
WATCH_ELEMENT <element_id>
    OK <subscription_id>
UNWATCH <subscription_id>
    OK
```

Out-of-band notifications use a new framing: between regular response lines, the agent may emit:
```
EVENT <subscription_id> <kind>\n
```
where `kind` is `process_exit`, `window_gone`, `element_invalidated`, `crash_dialog`. The MCP server surfaces these as tool errors to the model, mid-flight if necessary, with a clear "the target app crashed — abandon the current plan" message.

### Simpler interim alternative — pre-call validation

Before performing each high-cost action (`click`, `type_text`, `set_element_text`, `click_element`), the agent could implicitly verify the target is still alive:

- For `click_element` with a cached element id: re-fetch `BoundingRectangle` first; on `UIA_E_ELEMENTNOTAVAILABLE` return `ERR target gone`.
- For pixel `click(x, y)` after a recent `WINFOCUS`: confirm the focused window is still the same process; on mismatch return `ERR target gone`.
- For `type_text` after a focus operation: confirm the keyboard-focus owner is still the same process.

This is per-call rather than subscription-based — simpler to implement, no protocol additions, no event framing. The MCP server's tool wrappers do the validation; the AI sees `ERR target gone` and can decide to recover. Slightly more wasted tokens per call than a true async subscription, but covers 90% of the value.

### MCP-server-side rollup

Independent of agent-side detection: the MCP server can track "what process / window / element were the recent calls targeting?" and check vital signs **before** translating each tool call to a wire command. If a `WATCH_PROCESS` subscription fires (or a polling check fails), the next tool call returns an MCP error with `"target_app_crashed": true` rather than executing — saving the round-trip. The MCP server's session state is the natural home for this.

### Effort

- **Per-call validation** in agent + MCP wrappers: ~200 LOC, ~half-day. Lowest-cost first iteration.
- **Subscription verbs + event framing**: ~400 LOC, ~1 day. Add once the per-call approach hits its ceiling.
- **WER crash-dialog detection**: ~150 LOC, ~half-day. Cheap addition once basic detection works.

### Recommended order

Per-call validation first — most of the value, none of the protocol churn. Subscription support if and when streaming-style observation becomes a real need.

## Discoverability — windows-modern complete; backfill remaining

**Problem solved on `agents/windows-modern/`:** the operator no longer has to look up the VM's IP. The agent advertises itself via mDNS / DNS-SD when started with `REMOTE_HANDS_DISCOVERABLE=1` (or `--discoverable`); a scanner finds it on the LAN and prints `<ip>:<port>` plus the TXT-advertised metadata.

### What landed

- ✅ **`agents/windows-modern/discovery.cpp`** — minimal hand-rolled mDNS responder. Listens on `224.0.0.251:5353`, answers PTR queries for `_remote-hands._tcp.local.` with PTR + SRV + TXT + A in one packet. ~350 LOC, no third-party dependency. First non-loopback IPv4 from `GetAdaptersAddresses` is the advertised address.
- ✅ **Opt-in by design.** Default off; enable via `REMOTE_HANDS_DISCOVERABLE=1` env or `--discoverable` flag. INFO advertises `discovery=mdns` when active, `discovery=no` otherwise. The agent has no auth (per PROTOCOL.md), so this is documented as a trusted-LAN-only feature.
- ✅ **`client/hostctl-discover`** — stdlib-only Python scanner. Sends a PTR query, aggregates PTR/SRV/TXT/A records from the response, prints `<ip>:<port> <host> <txt-fields>`. `--first` exits on the first complete hit (useful in scripts), `--quiet` outputs just `<ip>:<port>` for piping into `hostctl`.

Example:
```
$ hostctl-discover
192.168.1.42:8765   windev-vm   os=windows-modern  protocol=1
192.168.1.51:8765   xp-vm-01    os=windows-nt      protocol=1
```

The MCP server can pipe this into its agent-target list at session start.

### Backfill

The responder code is OS-agnostic in design (UDP socket + DNS message parsing + interface enumeration). Direct ports needed:

- **`agents/windows-nt/`** — same logic, but with VC6-compatible C (no `<unordered_map>` or C++ containers; std::vector → manual realloc loop). `GetAdaptersAddresses` exists since Win 2000 so the interface enumeration works. Effort: ~half-day.
- **Future Linux/macOS targets** — `getifaddrs` instead of `GetAdaptersAddresses`; otherwise identical. Trivial port once those targets exist.

### Possible future polish (not blocking)

- **Unsolicited announcements at startup** (RFC 6762: send our records 1s and 5s after launch so caching browsers update faster). Currently we only respond to queries.
- **IPv6 (AAAA records)** — currently IPv4 only. Most LANs don't need it.
- **Conflict resolution** — if two agents claim the same instance name, RFC 6762 requires a probe-and-rename dance. Currently we just use the hostname; collisions silently produce two records and clients pick one. Acceptable for a small fleet.
- **Multi-interface advertising** — currently one interface. Multi-NIC hosts only advertise on the first non-loopback IPv4.

## WGC (`agents/windows-modern/`) — complete

All WGC work originally tracked here has landed. Every capture path on the modern target now uses Windows.Graphics.Capture when available, with BitBlt as a uniform fallback:

- ✅ **Per-call WGC** for `SHOT`, `SHOTRECT` within a single monitor, `SHOTWIN`.
- ✅ **Streaming WGC session** held open across `WATCH` / `WAITFOR` durations — frame retrieval drains the pool's latest frame, no per-frame setup cost.
- ✅ **Multi-monitor virtual-screen** capture via per-monitor WGC + BitBlt stitching into a virtual-screen-sized HBITMAP.
- ✅ **`SHOTRECT` via WGC + crop** — captures the parent monitor, crops client-side. Cross-monitor rects fall back to BitBlt.

See `agents/windows-modern/wgc.h` and `agents/windows-modern/COMPATIBILITY.md` for the per-path engine table.
