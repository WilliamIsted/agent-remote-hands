# windows-modern agent — OS compatibility

A 32 / 64-bit PE built against the Windows 10 SDK with explicit per-monitor V2 DPI awareness, virtual-screen multi-monitor capture, `SendInput`-based input, and Task Scheduler "at logon" task registration. Designed to be the right answer for **Vista and later**, where the windows-nt agent's assumptions break.

## Support matrix

| OS                                  | Status               | Notes |
|-------------------------------------|----------------------|-------|
| Windows Vista (SP2)                 | **Likely**           | Manifest declares Vista support; `SendInput`, virtual-screen metrics, Task Scheduler 2.0 all available. Untested. |
| Windows 7 (SP1)                     | **Likely**           | Same surface. Untested. |
| Windows 8 / 8.1                     | **Likely**           | Same surface. Untested. |
| Windows 10 1903+                    | **Full**             | Primary target. UTF-8 ACP and long-path manifest entries take effect from 1903. |
| Windows 11                          | **Full**             | Primary target. |
| Windows Server 2008 R2 / 2012 / 2016 / 2019 / 2022 | **Likely** | Same kernel branch as the corresponding client OS. Untested. |
| Windows XP / 2000 / 2003            | **No**               | Use `agents/windows-nt/` instead — those use APIs (`GetCursorInfo`, `LockWorkStation`) that windows-modern calls directly without runtime resolution; the binary won't load on pre-Vista. |
| Windows 9x / ME / 3.1               | **No**               | Modern PE, modern APIs. |

"Untested" means the design accommodates the OS but it hasn't been smoke-tested on a real install. The conformance suite is the way to verify each platform.

## Differences vs windows-nt — what works correctly here

The four problems windows-nt has on Vista+ are addressed:

1. **Service mode works.** The agent registers a Task Scheduler logon task instead of an SCM service. The task runs in the user's interactive session, sees their desktop, sends input that lands where the user can see it. No Session 0 isolation problem.
2. **DPI-correct.** Manifest declares Per-Monitor V2 awareness; the agent additionally calls `SetProcessDpiAwarenessContext` at startup as belt-and-braces. Coordinates and screenshots are in physical pixels regardless of the display's scale factor.
3. **Multi-monitor.** `SCREEN` reports the virtual-screen rectangle (`SM_CXVIRTUALSCREEN` × `SM_CYVIRTUALSCREEN`); `SHOT` captures across every connected display.
4. **Unicode input.** `KEYS` types via `SendInput` with `KEYEVENTF_UNICODE`, handling every Basic Multilingual Plane code point regardless of the active keyboard layout.

## Capture engine — WGC vs BitBlt

The agent uses **Windows.Graphics.Capture (WGC)** for every capture path whenever it's available (Windows 10 1803+, build 17134+, with a working D3D11 device — hardware or WARP). WGC handles the surfaces BitBlt misses: DirectComposition, Direct3D, full-screen games, DRM-protected video, modern WinUI 3 / XAML controls. BitBlt remains the fallback on every path so the agent stays functional on hosts where WGC isn't available.

| Path                                        | Engine                       | Notes |
|---------------------------------------------|------------------------------|-------|
| `SHOT` covering the full virtual screen     | WGC (per-monitor + stitch)   | Single-monitor: one WGC capture. Multi-monitor: one WGC capture per `EnumDisplayMonitors` result, stitched into a virtual-screen-sized HBITMAP. |
| `SHOTRECT <x> <y> <w> <h>` within one monitor | WGC + crop                 | Captures the parent monitor via WGC, crops to the requested rect. ~50 ms vs. BitBlt's ~5 ms — the trade-off is correctness on modern surfaces inside the rect. |
| `SHOTRECT` spanning multiple monitors       | BitBlt                       | Cross-monitor rects fall through; WGC's per-monitor model would require capturing N monitors and stitching just to crop, rarely worth it. |
| `SHOTWIN <title>`                           | WGC per-window               | `CreateForWindow` attaches directly to the HWND. Falls back to BitBlt of the window's screen rect on failure. |
| `WATCH` / `WAITFOR` thumbnails              | BitBlt                       | 32×32 downscale for change detection — the BitBlt path is fast and cursor-free, which is exactly what change detection needs. |
| `WATCH` / `WAITFOR` full frames             | WGC streaming session        | One session opened at the start of `WATCH` / `WAITFOR`, held across the duration, drained per-frame. Per-frame cost is the readback (~5 ms), not the setup. |
| Any path on a host where WGC init failed    | BitBlt                       | Win 10 < 1803, locked-down hosts without D3D11, hosts where `IsSupported()` returns false. |

`INFO` advertises `capture=wgc` when active; the listening banner also includes the engine choice.

## Multi-monitor

`SCREEN` reports the virtual-screen rectangle (all displays). Full virtual-screen capture stitches per-monitor WGC captures into a single HBITMAP — modern surfaces are correctly captured on **every** display, not just the primary. Single-monitor setups (the typical VM case) take a faster fast-path with one WGC capture and no stitching.

## Practical effect on a typical Win11 VM hosting Claude Code

- ✓ **Cmd, PowerShell, Notepad, classic Explorer, Win32 dialogs, Office** — always captured correctly (BitBlt also works for these).
- ✓ **Win11 Settings, Edge, Microsoft Store, Snipping Tool, Notification Center, modern Start menu, WinUI 3 / XAML** — captured correctly **via WGC** for `SHOT`, `SHOTRECT`, `SHOTWIN`, `WATCH`, and `WAITFOR`.
- ✓ **Full-screen Direct3D / games / DRM video** — captured correctly **via WGC**.
- ✓ **Streaming `WATCH` over modern surfaces** — runs at the configured interval without per-frame WGC setup cost; the modern shell renders correctly across the whole stream.

## INFO flags

The agent advertises the following on startup, which the MCP server / conformance suite read to know the right per-target configuration:

```
os=windows-modern arch=x64 protocol=1
capture=gdi multi_monitor=yes dpi_aware=yes cursor_in_shot=yes
input=sendinput
path_encoding=utf8 max_path=32767
windows=yes
user=<name> hostname=<machine>
auto_start=task power=<yes|no>
```

`capture=gdi` flips to `capture=wgc` once the WGC upgrade lands. `auto_start=task` distinguishes from windows-nt's `auto_start=service` so clients know which install mechanism was used.

## LAN discovery (opt-in)

When the agent is started with `REMOTE_HANDS_DISCOVERABLE=1` (or `--discoverable`), it advertises itself via mDNS / DNS-SD on the LAN. Service type `_remote-hands._tcp.local.`, port 8765, with TXT records `protocol=1` and `os=windows-modern`.

Scan from the controller side:

```
$ client/hostctl-discover
192.168.1.42:8765   windev-vm   os=windows-modern  protocol=1
```

Disabled by default. Reason: the agent has no authentication (the trust boundary is the network, per PROTOCOL.md). Broadcasting a remote-control surface on a network you don't trust is a footgun. Use only on trusted LANs (your home network, an isolated VM-host bridge, a corporate dev VLAN).

When active, `INFO` advertises `discovery=mdns`; otherwise `discovery=no`.

## Bottom line

**Use this build for Vista and later, not the windows-nt one.** It's correct on modern displays, correct on multi-monitor, correct in the user's interactive session via Task Scheduler, and ready for WGC-quality capture. The single missing piece is hardware-surface capture, which is a localised follow-up rather than a structural rewrite.
