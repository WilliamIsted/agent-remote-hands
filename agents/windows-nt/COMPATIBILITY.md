# windows-nt agent — OS compatibility

A 32-bit PE built with subsystem 4.00 against the Win32 ANSI API surface common to NT4 / 2000 / XP / Server 2003. Two axes determine compatibility per host: **does the binary load**, and **do all features behave correctly**.

## Support matrix

| OS                                      | Status               | Notes |
|-----------------------------------------|----------------------|-------|
| Windows 3.1 / 3.11                      | **No**               | 16-bit; can't load PE executables. Needs its own 16-bit agent (planned: `agents/windows-3.1/`). |
| Windows NT 3.1 / 3.5                    | **No**               | Subsystem-4.00 PE won't load. |
| Windows NT 3.51                         | **Partial**          | Loads. `PS` fails (no `tlhelp32` until NT4 SP4); service auto-restart fails (no `ChangeServiceConfig2` until NT5). Foreground works. |
| Windows NT 4 SP6                        | **Mostly**           | `IDLE` / `LOCK` return `ERR` (XP-era APIs, resolved dynamically and fall back gracefully when missing). Service auto-restart actions ignored. Otherwise functional. |
| Windows 95 (with Winsock 1.1)           | **Foreground only**  | No SCM → `--install` fails. No `GetLastInputInfo` → `IDLE` errors. No `LockWorkStation` → `LOCK` errors. `GetFileAttributesEx` is Win98+ → `STAT` / `LIST` may fail on stock 95. Run from a Startup-folder shortcut. |
| Windows 98 / 98 SE                      | **Foreground only**  | Same as 95 minus the `STAT` / `LIST` issue. No service, no `IDLE`, no `LOCK`. |
| Windows ME                              | **Foreground only**  | Same as 98. |
| Windows 2000                            | **Full**             | Everything works including service auto-restart. |
| Windows XP                              | **Full**             | Original target. |
| Windows Server 2003                     | **Full**             | Same code path as XP. |
| Windows Vista / 7 / 8 / 8.1 / 10 / 11   | **Loads, but degraded** | See below. Use `agents/windows-modern/` (planned) instead. |

## Vista-and-later — why it loads but isn't right

The binary opens and serves traffic, but four behaviours diverge from earlier Windows in ways that matter for an agent whose job is driving a UI:

1. **Service mode is silently broken.** `SERVICE_INTERACTIVE_PROCESS` is still accepted by the SDK, but Vista's Session 0 Isolation moved services to a separate, headless desktop. The agent installs and runs — but its synthesised keystrokes / mouse moves / screenshots target Session 0, not the user's interactive session. From the user's perspective, nothing visible happens.
   - **Workaround until `agents/windows-modern/` lands:** don't use `--install` on Vista+. Run the binary in foreground (Startup-folder shortcut, scheduled task at logon, or just start it manually).
2. **No DPI awareness.** On HiDPI displays, coordinates and screenshots refer to a virtualised 96-DPI grid, not physical pixels. Effect: clicks land in the wrong place on scaled monitors, and screenshots are blurry / lower-res than the display.
3. **`BitBlt`-based capture misses hardware-accelerated and secure surfaces.** DRM video, some DirectComposition windows, and full-screen Direct3D content render as black rectangles in screenshots.
4. **Multi-monitor wrong.** Only the primary display is reported by `SCREEN`; `SHOT` covers only the primary. `SHOTRECT` with negative coordinates targeting a secondary monitor will return a black bitmap.

These four points are exactly what the planned `agents/windows-modern/` build closes (via `SendInput`, `Windows.Graphics.Capture`, `SetProcessDpiAwarenessContext`, virtual-screen metrics, and Task-Scheduler-at-logon registration in place of SCM).

## What "Foreground only" means

On 9x and ME the agent has no SCM to register against, but the binary itself runs fine. To get auto-start without a service:

- Drop a shortcut to `remote-hands.exe` in `%USERPROFILE%\Start Menu\Programs\Startup\`.
- The agent starts when the user logs in, dies when they log out. Acceptable for single-user dev/test VMs, less so for unattended targets.

For unattended 9x/ME, the only options are third-party "service-style" wrappers (e.g. `srvany`-equivalents that work on 9x) or running the binary under a kiosk launcher.

## Per-feature degradation summary

| Feature                  | NT4 SP6 | Win 9x / ME | Win 2000 / XP / 2003 | Vista+        |
|--------------------------|---------|-------------|----------------------|---------------|
| Foreground server        | ✓       | ✓           | ✓                    | ✓             |
| `--install` (service)    | partial | **no SCM**  | ✓                    | broken (S0)   |
| Auto-restart on crash    | ✗       | n/a         | ✓                    | n/a (broken)  |
| `IDLE`                   | ✗       | ✗           | ✓                    | ✓             |
| `LOCK`                   | ✗       | ✗           | ✓                    | ✓             |
| `STAT` / `LIST`          | ✓       | 98+ ✓ / 95 ✗| ✓                    | ✓             |
| `PS`                     | SP4+ ✓  | ✓           | ✓                    | ✓             |
| `SHOT` (correct pixels)  | ✓       | ✓           | ✓                    | DPI / surfaces wrong |
| Multi-monitor `SHOT`     | ✗       | ✗           | ✗ (primary only)     | ✗             |
| Power verbs (gated)      | ✓       | ✓           | ✓                    | ✓             |

## Bottom line

The windows-nt binary is a complete, fault-tolerant agent for **NT4 SP6 → Server 2003**. Earlier NT and the 9x/ME line work in foreground-only / reduced modes. Vista+ runs but should not be the deployment target — the four issues above will surface in any non-trivial use, and `agents/windows-modern/` exists specifically to address them.
