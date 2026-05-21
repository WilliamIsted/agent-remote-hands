# macos-modern agent

macOS target for Agent Remote Hands. Implements the v2 wire protocol — see [`PROTOCOL.md`](../../PROTOCOL.md) at the repo root for the canonical spec.

**Status:** MVP slice with first real verb — `connection.{hello,close,reset,tier_drop}`, `system.{info,health,capabilities,verbs}`, and `screen.capture`. Everything else returns `ERR not_supported_by_target`.

**Floor:** macOS 14 Sonoma (raised from the planning's Ventura 13 once `CGWindowListCreateImage` proved unavailable in the macOS 26 SDK — `SCScreenshotManager.captureImage` is now the primary capture API and lands at Sonoma).

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
.build/release/rha-mac                 # Default: TCP 8765 on 127.0.0.1
.build/release/rha-mac --port 9000     # Custom port
.build/release/rha-mac --host 0.0.0.0  # Expose to LAN
```

The agent does not yet generate a token file or advertise via Bonjour — those land in later slices.

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

| Verb | Status |
|---|---|
| `connection.hello` | ✓ |
| `connection.close` | ✓ |
| `connection.reset` | ✓ |
| `connection.tier_raise` | stub (returns `ERR not_supported_by_target` until token file lands) |
| `connection.tier_drop` | ✓ (no-op at read tier; rejects invalid tiers) |
| `system.info` | ✓ — advertises `capture=screencapturekit`, image formats, TCC state |
| `system.health` | ✓ |
| `system.capabilities` | ✓ (lists only implemented verbs) |
| `system.verbs` | ✓ |
| `screen.capture` | ✓ — full-screen only; `--format png\|jpeg\|heic\|bmp`, `--quality 1-100`. `--region`/`--window`/`--monitor` return `ERR invalid_args` (later slice). `webp` returns `ERR unsupported_format`. |
| Everything else | `ERR not_supported_by_target` |

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
