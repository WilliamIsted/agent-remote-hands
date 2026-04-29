# Windows automation notes

Operational gotchas that aren't agent bugs but cost time. None of these are
unique to Agent Remote Hands — they apply to any Windows automation tool —
but they're cheap to document and save callers a diagnostic round.

The deeper, agent-specific behaviours (UIPI, foreground locks, wire desync)
live in [`PROTOCOL.md`](../PROTOCOL.md). This file collects the
**OS-level** footguns that surface in real automation flows.

## MSI installs serialise via a global mutex

Windows Installer (`msiexec`) holds a machine-wide mutex while a `/i`
operation is in progress. A second concurrent `msiexec /i` returns exit code
**1618** (`ERROR_INSTALL_ALREADY_RUNNING`) and refuses to start.

The mutex spans **all** MSI operations on the box, including those started
by other users or by Windows Update / WSUS in the background. So even on a
fresh VM you can occasionally see 1618 if the OS is mid-update.

**Don't parallelise MSI installs.** Sequence them:

```
process.start  msiexec.exe /i \"C:\\path\\firefox.msi\" /quiet
process.wait   <pid> 600000
process.start  msiexec.exe /i \"C:\\path\\thunderbird.msi\" /quiet
process.wait   <pid> 600000
```

If you must parallelise across hosts, run each on a separate machine — the
mutex is local. NSIS installers (Mozilla's preferred shape, plus most
freeware) and ClickOnce do **not** share the mutex; they can run in
parallel. Steam's installer is its own process tree, also unconstrained.

## Session 0 isolation

When commands are run via WinRM (`Invoke-Command`, `Enter-PSSession`),
PsExec without `-i`, or as a service account without an interactive logon,
they land in **Session 0** — a non-interactive session with no desktop, no
window station that can render UI, and no user input source.

Symptom: `Start-Process steam.exe` "succeeds" with exit 0 but the user
sees no window. `taskmgr` reports steam.exe running, but with `Session: 0`
where interactive apps live in `Session: 1`.

Agent Remote Hands itself runs in the logged-on user's session (Task
Scheduler logon-task — the agent appears in `Session: 1+`), so verbs like
`process.start` spawn children in the interactive session by default. The
problem only appears if a caller bridges in via WinRM and tries to talk to
the agent from there — at which point the bridge process is in Session 0
but the agent is in Session 1, and that mismatch is fine for TCP traffic
but breaks for any direct CreateProcess invocation done from the WinRM side.

Fixes when you genuinely need to drive a Session 0 → Session 1 bridge:

- `schtasks /Run /TN <task>` against a task already registered for the
  interactive session
- PsExec `-i 1` to spawn into the interactive session explicitly
- Use the agent's wire protocol — it's already in the right session

## Foreground-lock policy

Windows blocks `SetForegroundWindow` from any process that doesn't already
own the foreground (or that hasn't been blessed by `AllowSetForegroundWindow`
from the current foreground process). This is anti-focus-stealing
protection.

Agent Remote Hands surfaces denial as `ERR lock_held {"lock_type":"foreground"}`
on `window.focus`. Common-case fix: ensure the current foreground process
(usually `explorer.exe` on a fresh logon) has called `AllowSetForegroundWindow`
or that the requesting process has been registered via the same mechanism.
The agent already calls `AllowSetForegroundWindow(ASFW_ANY)` before
`SetForegroundWindow`; that handles the idle-desktop case but not the
case where another app has just stolen focus.

If a verb returns `ERR lock_held` repeatedly:

1. Check `system.info.integrity` — UIPI may also be in play (see
   [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels)). A
   higher-IL window simply isn't reachable from a lower-IL agent.
2. Use `screen.capture` and `element.list` to confirm the target is
   actually drawn — lock denial sometimes correlates with the window
   not yet being interactable.
3. Retry after a short delay. The lock window is per-input-event; a 250 ms
   pause often clears it.

## Integrity levels and UIPI

The biggest non-obvious failure mode for installer automation. Covered in
detail at [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels).
Summary:

- The agent runs at Medium IL when started via Task Scheduler logon-task
- Most installers (NSIS, MSI, Steam) auto-elevate to High IL
- UIPI silently drops cross-IL synthesised input — the agent surfaces this
  as `ERR uipi_blocked` rather than a silent `OK`
- Workarounds: spawn a second elevated agent, sign with `uiAccess="true"`,
  or run the installer's logon-task as an admin user

If you find yourself wondering why a click "worked" but the wizard didn't
advance, this is the first thing to check.

## Cross-references

- Wire protocol: [`PROTOCOL.md`](../PROTOCOL.md)
- UIPI / integrity levels: [`PROTOCOL.md` §8](../PROTOCOL.md#8-elevation-and-integrity-levels)
- Foreground lock surfaces: [`PROTOCOL.md` §10.4](../PROTOCOL.md#104-foreground-locks)
- Wire desync recovery: [`PROTOCOL.md` §10.6](../PROTOCOL.md#106-wire-desync-recovery)
