# macos-modern compatibility

What this binary covers, what it requires at runtime, and what it gracefully degrades on.

## Naming

This build ships as **`rha-mac.modern.universal2`** — the naming convention is `rha-mac.<family>.<arch>`. No `.exe` extension on Darwin. Universal 2 is the default; slim `rha-mac.modern.arm64` and `rha-mac.modern.x86_64` variants are optional outputs.

## What "modern" means

The label `macos-modern` refers to the **Monterey-and-later** family of macOS — 12.3 and up, where Swift concurrency is native and the full TCC consent category set (Screen Recording, Accessibility, Input Monitoring) is present and stable.

## API floor (current build)

| Capability | Minimum macOS | Notes |
|---|---|---|
| TCP listener (BSD sockets) | 10.4 Tiger | POSIX `socket`/`bind`/`listen`/`accept` |
| Wire framing | n/a | Pure Swift |
| `Foundation` (JSON, Date, hostnames) | 10.10 Yosemite | `JSONSerialization`, `ProcessInfo` |
| Swift concurrency (`async`/`await`) | 12.0 Monterey | Native runtime; no back-deployment libraries bundled — the hard technical floor |
| `CGDisplayCreateImage` (screen capture) | 10.6 Snow Leopard | Synchronous CoreGraphics; `dlsym`-resolved (absent from the macOS 26 SDK) |
| `ImageIO` encoders (PNG/JPEG/HEIC/BMP) | 10.4 Tiger | `CGImageDestination*` |
| Screen Recording TCC probe | 10.15 Catalina | `CGPreflightScreenCaptureAccess` |

The hard technical floor is **macOS 12.0**, set by the Swift concurrency runtime — the agent uses `async`/`await` throughout and bundles no back-deployment libraries. The deployment target is pinned slightly higher, at **12.3 Monterey**, as the deliberate family target from the planning trade-off analysis. `screen.capture` uses the synchronous `CGDisplayCreateImage` CoreGraphics path: ScreenCaptureKit (`SCShareableContent` / `SCStream`) was evaluated and rejected — its async machinery requires a running main run loop, which this headless server does not provide, so the calls hang. `CGWindowListCreateImage` was also tried and returns `nil` in this process context; `CGDisplayCreateImage` grabs the display framebuffer and works. Big Sur 11 was considered and rejected — see the planning trade-off analysis.

## Full-surface API floor

| Capability | Minimum macOS | Notes |
|---|---|---|
| Screen capture | 10.6 Snow Leopard | `CGDisplayCreateImage`, synchronous CoreGraphics, `dlsym`-resolved (absent from the macOS 26 SDK). ScreenCaptureKit is not used — its async API hangs without a main run loop |
| Swift concurrency | 12.0 Monterey | `async`/`await` runtime; native, no back-deployment — the hard technical floor |
| AX UI Automation | 10.2 Jaguar | `AXUIElementCreateApplication`, `AXObserver` |
| Synthetic input | 10.4 Tiger | `CGEvent` family |
| TCC: Accessibility | 10.9 Mavericks | `AXIsProcessTrustedWithOptions` |
| TCC: Screen Recording | 10.15 Catalina | `CGPreflightScreenCaptureAccess` |
| TCC: Input Monitoring | 10.15 Catalina | `IOHIDCheckAccess` |
| Clipboard | 10.0 | `NSPasteboard.general` |
| Image encoding | 10.4 Tiger | `ImageIO` / `CGImageDestination` |
| Vision OCR | 10.15 Catalina | `VNRecognizeTextRequest`. Revision 3 is requested when running on Sonoma 14+; Vision picks its OS-native default on Monterey/Ventura (Rev 2) |
| Power assertions | 10.5 Leopard | `IOPMAssertion` |
| Bonjour mDNS | 10.0 | `NetService` / `dns_sd.h` |
| Token entropy | 10.7 Lion | `SecRandomCopyBytes` |
| Service mode | 10.10 Yosemite | launchd LaunchAgent (NOT LaunchDaemon — needs Aqua session) |

The hard technical floor is **macOS 12.0** (Swift concurrency); the deployment target is pinned at **12.3 Monterey** as the deliberate family target. Everything else is comfortably below.

## Architecture

Universal 2: arm64 + x86_64. Both slices are first-class:

- arm64 — Apple Silicon (M-series), shipped 2020+.
- x86_64 — Intel Macs running Monterey 12.3+ (Mac Pro 2019, iMac Pro 2017, MacBook Pro 16" 2019, etc.). The Intel ceiling tightens with each macOS release; check `system_profiler SPSoftwareDataType` for the host's compatibility.

## Code signing

Unsigned local builds run from the build directory work for development. Distribution builds require Hardened Runtime + a Developer ID Application certificate + notarisation through Apple's notary service.

Required Info.plist usage descriptions (when the corresponding verbs land):

- `NSScreenCaptureUsageDescription`
- `NSAccessibilityUsageDescription`
- `NSAppleEventsUsageDescription`
