# macos-modern agent

macOS target for Agent Remote Hands. Implements the v2 wire protocol — see [`PROTOCOL.md`](../../PROTOCOL.md) at the repo root for the canonical spec.

**Status:** Full 88-verb v2.2 wire surface, MCP-stdio framing post-hello, token + tier_raise, subscription mechanism for `watch.*` with EVENT framing, AX + CoreGraphics capture + CGEvent + NSPasteboard + ImageIO + Vision + FSEvents + IOPMAssertions integrated. **Conformance suite: 101/238 tests pass against the live agent** as of the latest commit; remaining 59 failures + 78 skips are family-specific (see [Known divergences](#known-divergences) below).

**Floor:** macOS 12.3 Monterey — the deliberate target from the floor-tradeoffs analysis (see [`COMPATIBILITY.md`](COMPATIBILITY.md), Big Sur 11 considered and rejected). `screen.capture` uses the synchronous `CGDisplayCreateImage` CoreGraphics path (resolved via `dlsym`); ScreenCaptureKit is not used — its async API hangs without a main run loop, which this headless server does not provide. No single API hard-requires 12.3: the hard technical floor is Swift concurrency (macOS 12.0), and 12.3 is kept as a conservative documented target.

## Build

Requires Swift 5.9+ (Xcode 15+ or a standalone toolchain). SwiftPM is the build system.

```bash
swift build -c release
```

Output: `.build/release/rha-mac`.

A Universal 2 binary (arm64 + x86_64) is produced via:

```bash
swift build -c release --arch arm64 --arch x86_64
```

## Run

```bash
.build/release/rha-mac                 # Default: TCP 8765 on 127.0.0.1, Bonjour on
.build/release/rha-mac --port 9000     # Custom port
.build/release/rha-mac --host 0.0.0.0  # Expose to LAN
.build/release/rha-mac --no-discoverable  # Don't publish via Bonjour
```

A fresh token is generated at `~/Library/Application Support/AgentRemoteHands/token` on each start. Use it to elevate from `read` to higher tiers via `connection.tier_raise`.

## Install as a LaunchAgent

For autostart on login, run from a clean shell:

```bash
Tools/install-macos-modern.sh                          # default port 8765
Tools/install-macos-modern.sh --port 9000              # custom port
Tools/install-macos-modern.sh --uninstall              # reverse
```

The script writes `~/Library/LaunchAgents/me.isted.rha.plist` (LaunchAgent, not LaunchDaemon — needs the user's Aqua session), copies the release binary to `~/Applications/rha-mac/rha-mac`, and `launchctl bootstrap`s it. Stdout/stderr land at `~/Library/Logs/rha-mac.{out,err}.log`. `KeepAlive { Crashed: true }` restarts the agent on crash but not on clean exit.

## Sign + notarise for distribution

```bash
Tools/sign-macos-modern.sh \
    --identity "Developer ID Application: <Your Name> (TEAMID)" \
    --profile NOTARY_PROFILE \
    --output ./dist/rha-mac.modern.universal2
```

Requires a Developer ID Application cert in the keychain plus `notarytool` credentials stored via `xcrun notarytool store-credentials`. Builds Universal 2, signs with Hardened Runtime + the [`rha-mac.entitlements`](rha-mac.entitlements) file (`network.server` + `automation.apple-events`), submits to Apple's notary service, and staples the ticket on success.

## Test

```bash
swift test
```

## Smoke test

In one terminal:

```bash
swift run rha-mac --port 8765
```

In another:

```bash
# Hello
printf 'connection.hello smoke-test 2\n' | nc 127.0.0.1 8765

# Then in the same connection:
#   system.info  → JSON identity blob
#   system.health → OK 0
#   connection.close → OK 0 + socket drop
```

A guided smoke-test session is in [`Tools/smoke-test-macos.sh`](../../Tools/smoke-test-macos.sh) *(planned)*.

## What's implemented

All 13 verb namespaces; 82 implementing verbs + 6 stubs = 88 total in `system.capabilities`. Windows-only verbs (`registry.*`, `watch.registry`, `input.send_message`, `input.post_message`) are intentionally omitted from `system.capabilities` per PROTOCOL.md §3.2 ("a verb absent from this map is not implemented") — clients see them as unsupported via the standard discovery flow.

| Namespace | Status |
|---|---|
| `connection.*` (5) | All live; tier_raise uses 256-bit token rotated on each agent restart |
| `system.*` (12) | info/health/capabilities/verbs live; power.{shutdown,reboot,logoff,hibernate,sleep,lock,blockers} live; power.cancel stub (no --delay timer yet) |
| `screen.capture` | `CGDisplayCreateImage` (synchronous CoreGraphics, `dlsym`-resolved); full-screen only. `--region`/`--window`/`--monitor` reserved for follow-up slice |
| `window.*` (6) | CGWindowList enumeration; AX for focus/close/move/state |
| `input.mouse.*` (6) | CGEvent + Input Monitoring TCC |
| `input.keyboard.*` (4) | CGEvent + virtual-key lookup table + Unicode `type` |
| `input.position` | CGEvent.location |
| `element.*` (14) | AX API via per-connection element table; all read + update verbs functional |
| `file.*` (10) | POSIX + Foundation (incl. URLSession for download, FSEvents for wait) |
| `directory.*` (6) | POSIX + Foundation |
| `process.*` (5) | sysctl + libproc + NSWorkspace + kqueue NOTE_EXIT |
| `vision.ocr` | VNRecognizeTextRequest (revision 3 on Sonoma+) |
| `watch.*` (7) | Subscription registry + EVENT framing. Process/file/window/element/region all wired; FSEvents delivery needs follow-up (see Slice 13 notes) |
| `registry.*` + `watch.registry` + `input.{send,post}_message` | Omitted from capabilities (Windows-only concepts) |

## Known divergences

The conformance suite (`tests/conformance/`) was written assuming a Windows agent. The macos-modern build passes **101/238 tests**; the remaining 59 failures and 78 skips fall into these categories:

- **Family enum** — the conformance suite's `KNOWN_FAMILIES` set is `{"windows-modern", "windows-classic", "windows-legacy"}`; macos-modern fails the family-known assertion by construction. Tests: `test_info_family_is_known`.
- **Window handle prefix** — the suite asserts handles begin with `win:`; macOS uses `mac:<CGWindowID>`. Tests: `test_window_list_entries_have_required_fields`, several others.
- **TCC-gated reads** — `screen.capture`, `window.list` (off-process titles), and AX walks need TCC grants the dev binary doesn't always hold. Tests under `test_screen.py`, parts of `test_window.py`.
- **WebSocket framing** — `--framing ws` isn't implemented; all of `test_websocket.py` fails. macOS family planning treats WS as a follow-up.
- **Windows-shaped argument shapes** — a handful of tests check arg validation against Windows-specific arg orderings or flags that macOS doesn't use.

These are documented divergences rather than agent bugs; a follow-up slice would either move the relevant assertions behind family-aware skips in the conformance suite, or note them per-test in `tests/conformance/KNOWN-DIVERGENCES.md`.

## TCC: Screen Recording grant

`screen.capture` requires the Screen Recording TCC grant. The probe uses `CGPreflightScreenCaptureAccess()` and `screen.capture` returns `ERR permission_denied {"category":"screen_recording","hint":"…"}` if the grant is absent.

**Granting on a dev machine:**

1. Run the binary at least once (`swift run rha-mac --port 8765`).
2. Open **System Settings → Privacy & Security → Screen Recording**.
3. Find `rha-mac` (or `swift-frontend` if running via `swift run` — the embedded helper path varies). Toggle on.
4. Restart the agent so the grant takes effect for the new process.

For a stable signing identity (Developer ID Application + Hardened Runtime), the grant persists across rebuilds. Unsigned dev binaries reset whenever the binary's path or hash changes.

## Host vs VM

For windows-modern the agent must NEVER run on the dev host (see top-level `CLAUDE.md`). The MVP macOS slice is read-only and benign — `system.info`/`system.health` etc. have no side effects — so running on the dev host is fine for now. The rule will tighten once `input.*`, `process.*`, `system.power.*` land.

## File layout

```
agents/macos-modern/
├── Package.swift                              SwiftPM manifest
├── README.md / COMPATIBILITY.md
├── Sources/
│   ├── RemoteHandsCore/                       library
│   │   ├── Wire.swift                         frame parse + response writers + tier enum
│   │   ├── SystemInfo.swift                   system.info JSON construction
│   │   ├── Verbs.swift                        verb registry + handlers
│   │   ├── Connection.swift                   per-connection state machine
│   │   └── Server.swift                       BSD-socket accept loop
│   └── rha-mac/
│       └── main.swift                         CLI + bootstrap
└── Tests/RemoteHandsCoreTests/
    └── WireTests.swift                        frame parsing tests
```
