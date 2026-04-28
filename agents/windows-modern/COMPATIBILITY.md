# windows-modern compatibility

What this binary covers, what it requires at runtime, and what it gracefully degrades on.

## Naming

This is the **edge target** — the unsuffixed `remote-hands.exe`. When a successor target supersedes it, this build will be renamed at the handover. Until then, "modern" is the default.

## What "modern" means

The label `windows-modern` refers to the **MIC-aware** family of Windows — the post-Vista lineage where mandatory integrity control, UAC, and UIPI exist. It is *not* tied to any single Windows version.

The current build targets Win 10 / 11 in practice (that's what's been tested) and compiles against `_WIN32_WINNT=0x0A00`. Most of the Win32 surface it actually uses lands at Vista, so a back-port to Vista–8.1 is realistic — the open work is runtime feature detection plus a smaller test pass.

## API floor (current build)

| Capability | Minimum Windows | Notes |
|---|---|---|
| Wire framing, TCP listener | NT 4 (in principle); tested Win 10+ | Plain Winsock |
| Token entropy (`BCryptGenRandom`) | Vista | Replaces older `CryptGenRandom` |
| Synthetic input (`SendInput`) | Win 2000 | Unicode keystrokes from XP |
| Window enumeration (`EnumWindows`, `GetWindowRect`) | NT 3.1 | |
| Foreground-lock detection (`GetForegroundWindow`) | NT 3.1 | |
| Workstation lock (`LockWorkStation`) | Win 2000 | |
| Mandatory integrity probes (`TokenIntegrityLevel`) | Vista | |
| UI Automation (`IUIAutomation`) | Vista (full pattern set: Win 7+) | |
| Screen capture (`BitBlt` + `GetDIBits`) | NT 3.1 | |
| `PrintWindow PW_RENDERFULLCONTENT` | Win 8.1 | Falls back to plain `PrintWindow(0)` on older |
| PNG encoding (WIC) | Vista | Built into Windows |
| Shutdown reason text (`ShutdownBlockReasonQuery`) | Vista | |
| Power verbs (`ExitWindowsEx` / `InitiateShutdown`) | NT 4 / Win 2000 | Requires `SeShutdownPrivilege` |
| Hibernate / sleep (`SetSuspendState`) | Win 2000 | |
| File watching (`ReadDirectoryChangesW`) | NT 4 | |
| Registry watching (`RegNotifyChangeKeyValue`) | NT 4 | |
| Adapter enumeration (`GetAdaptersAddresses`) | Win 2000 | mDNS responder uses this |
| `schtasks` self-install | XP+ | Used by `--install` |
| `netsh advfirewall firewall` | Vista+ | Used by `--install` |

The hardest floor in the current code is **Vista** (`BCryptGenRandom`, `TokenIntegrityLevel`, `netsh advfirewall`). If a future build wants to extend down to XP/2003, those three are the things that need adapter shims or fall-backs.

## Capability advertisement

`system.info.capabilities` is built at runtime and reflects what's actually available on the host:

| Field | Today's value | Notes |
|---|---|---|
| `capture` | `"gdi"` | BitBlt path. WGC fast path is a future runtime probe. |
| `ui_automation` | `"uia"` | IUIAutomation. |
| `image_formats` | `["png", "bmp"]` | WebP arrives when libwebp is bundled. |

Older Windows versions where some of these would degrade get the smaller advertisement. Clients gate verbs on the advertised set rather than on a Windows version string.

## Deferred / not yet wired

- **WGC** (`Windows.Graphics.Capture`) — fast capture for Win 10 1803+. Spec-allowed but currently the agent always uses BitBlt.
- **WebP encoding** — needs libwebp; until then PNG is the lossless format and BMP is the no-encode fallback.
- **Vista–8.1 back-port** — code path mostly applicable but not tested. The runtime feature probes that would gate WGC etc. would also gate any future Vista-only quirks.

## Tested

The current build is regularly tested against:

- Windows 11 23H2 (`_WIN32_WINNT=0x0A00`).
- Windows 10 22H2.

Earlier MIC-era versions (Vista, 7, 8, 8.1) are not currently part of the test loop.

## What forces a target rename

A future generation of Windows that *changes the security model* — not merely "adds new APIs" — would justify a new target. APIs evolve every release; security models change roughly once a decade. Concrete examples that would warrant a successor target:

- A Windows release that removes mandatory integrity control or replaces UIPI with a new mechanism.
- A Windows release that mandates per-app sandboxing for any native binary (currently optional via AppContainer).
- A move to a fundamentally new privilege model (e.g. capability-based replacing ACLs).

Adding `WGC2` or a new IL level isn't a successor — that's runtime detection added to this binary.
