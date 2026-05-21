# macos-modern compatibility

What this binary covers, what it requires at runtime, and what it gracefully degrades on.

## Naming

This build ships as **`rha-mac.modern.universal2`** — the naming convention is `rha-mac.<family>.<arch>`. No `.exe` extension on Darwin. Universal 2 is the default; slim `rha-mac.modern.arm64` and `rha-mac.modern.x86_64` variants are optional outputs.

## What "modern" means

The label `macos-modern` refers to the **ScreenCaptureKit-era** family of macOS — Ventura 13.0 and later, where the modern hardware-accelerated capture API is GA and the full TCC consent category set (Screen Recording, Accessibility, Input Monitoring) is present and stable.

The current MVP slice does not yet use ScreenCaptureKit, AX, or CGEvent — it's a wire-protocol server only. Those APIs land as the corresponding verbs are implemented.

## API floor (current MVP slice)

| Capability | Minimum macOS | Notes |
|---|---|---|
| TCP listener (BSD sockets) | 10.4 Tiger | POSIX `socket`/`bind`/`listen`/`accept` |
| Wire framing | n/a | Pure Swift |
| `Foundation` (JSON, Date, hostnames) | 10.10 Yosemite | `JSONSerialization`, `ProcessInfo` |
| `ScreenCaptureKit.SCScreenshotManager` | 14.0 Sonoma | One-shot capture; sets the family floor |
| `ImageIO` encoders (PNG/JPEG/HEIC/BMP) | 10.4 Tiger | `CGImageDestination*` |
| Screen Recording TCC probe | 10.15 Catalina | `CGPreflightScreenCaptureAccess` |

The hardest current floor is **macOS 14 Sonoma**, set by `SCScreenshotManager.captureImage` (the simplest one-shot capture API). The planning originally targeted Ventura 13, but implementation discovered `CGWindowListCreateImage` is unavailable in the macOS 26 SDK — Apple removed it entirely after deprecating it in Sequoia. The remaining ScreenCaptureKit path on Ventura 13 is `SCStream`-based, requires an async frame-callback wrapper, and is omitted from the MVP for simplicity. A Ventura-supporting slice can re-add it later if demand emerges.

## Planned API floor (full surface)

| Capability | Minimum macOS | Notes |
|---|---|---|
| ScreenCaptureKit | 13.0 Ventura | `SCStream`, `SCScreenshotManager` |
| CoreGraphics capture | 10.0 | `CGWindowListCreateImage` — fallback path (deprecated in Sequoia 15) |
| AX UI Automation | 10.2 Jaguar | `AXUIElementCreateApplication`, `AXObserver` |
| Synthetic input | 10.4 Tiger | `CGEvent` family |
| TCC: Accessibility | 10.9 Mavericks | `AXIsProcessTrustedWithOptions` |
| TCC: Screen Recording | 10.15 Catalina | `CGPreflightScreenCaptureAccess` |
| TCC: Input Monitoring | 10.15 Catalina | `IOHIDCheckAccess` |
| Clipboard | 10.0 | `NSPasteboard.general` |
| Image encoding | 10.4 Tiger | `ImageIO` / `CGImageDestination` |
| Vision OCR | 10.15 Catalina | `VNRecognizeTextRequest` |
| Power assertions | 10.5 Leopard | `IOPMAssertion` |
| Bonjour mDNS | 10.0 | `NetService` / `dns_sd.h` |
| Token entropy | 10.7 Lion | `SecRandomCopyBytes` |
| Service mode | 10.10 Yosemite | launchd LaunchAgent (NOT LaunchDaemon — needs Aqua session) |

The full-surface floor is **macOS 13 Ventura** (set by ScreenCaptureKit). Everything else is comfortably below.

## Architecture

Universal 2: arm64 + x86_64. Both slices are first-class:

- arm64 — Apple Silicon (M-series), shipped 2020+.
- x86_64 — Intel Macs running Ventura 13+ (Mac Pro 2019, iMac Pro 2017, MacBook Pro 16" 2019, etc.). The Intel ceiling tightens with each macOS release; check `system_profiler SPSoftwareDataType` for the host's compatibility.

## Code signing

Unsigned local builds run from the build directory work for development. Distribution builds require Hardened Runtime + a Developer ID Application certificate + notarisation through Apple's notary service.

Required Info.plist usage descriptions (when the corresponding verbs land):

- `NSScreenCaptureUsageDescription`
- `NSAccessibilityUsageDescription`
- `NSAppleEventsUsageDescription`
