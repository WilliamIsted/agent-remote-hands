# Origins of Agent Remote Hands

**Status:** Historical artefact. Pre-dates Agent Remote Hands as a project. Preserved for lineage record only.

**Do not implement against this.** The verb surface, env-var names, and binary naming all changed during the extraction into the V1 windows-nt agent. Use [`v0.1.0`](https://github.com/WilliamIsted/agent-remote-hands/releases/tag/v0.1.0) for V1, or [`v0.2.x`](https://github.com/WilliamIsted/agent-remote-hands/releases/tag/v0.2.0-rc.5) for current.

## What this is

The seed code that became Agent Remote Hands. A single-file C TCP control agent for a Windows XP build VM, plus its host-side Python control client (`vmctl`), companion VC6 build script, and the period-correct `vm_agent.exe` binary built from the source preserved here.

These files originate from an internal VC6 game-port project — specifically, its `tools/vm_agent/` directory — and are preserved at the two upstream commits that constitute the seed's complete creation arc, captured here as a partial-commit history pair.

## Origin

**Agent Remote Hands was born out of frustration with `vncdo`.**

On **27 April 2026** the author was using `vncdo` from a Mac host to drive three parallel Windows XP build VMs as part of porting a GoG-released open-source game's VC6 codebase to a runnable Windows 10 / 11 build. Each VM held VC6, the original game source (a German source-directory path containing `ü`), and a build of the port-in-progress. Claude Opus 4.7 (1M context, co-author throughout) was reading VNC-captured screenshots to infer game state during automated playtests after each rebuild — the kind of full-game-loop verification the port needed to flush out crashes the build itself didn't catch.

The frustration was structural, not incidental. `vncdo` is a fine command-line VNC client, but driving a long build/test/debug loop through it surfaced limit after limit:

- **Per-frame overhead.** Every screenshot was a fresh VNC framebuffer fetch. Multiple shots per playtest turn into multi-second tax.
- **Indirect input model.** "Click here, then re-read pixels to confirm" — every click required a follow-up screenshot to verify state, doubling the round-trip count.
- **No file I/O.** Pulling a crash log (`C:\at_exc.log`) or pushing a patched config required `scp` / shared-folder hops on top of `vncdo`.
- **No process control.** Launching the game executable, waiting for it to exit, parsing the exit code — none of that lived in `vncdo`. More tooling to glue.
- **Encoding limits.** Paths with non-ASCII characters (the German game source) didn't round-trip cleanly through `vncdo`'s command pipeline.

`vm_agent` was the response. A single-socket TCP control channel exposing exactly what the build/test/debug loop actually needed: keystrokes (with chords), mouse (move/click/drag/wheel/down/up), screenshots, file I/O, process exec/wait — all in one connection, all length-prefixed, all binary-safe. The original commit message frames it as "a superset of vncdo" — same automation surface, no per-frame overhead, plus the file/process/encoding affordances vncdo never had.

That specific use case — automated playtest of a Win32 game after each rebuild — explains the verb shape preserved here:

- `SHOT` — capture the screen so Claude could see game UI state mid-test
- `MPOS`, `MOVE`, `MOVEREL`, `CLICK`, `DCLICK`, `MDOWN`/`MUP`, `DRAG`, `WHEEL` — full mouse vocabulary for clicking through the game's menus and game-board interaction
- `KEY`, `KEYS`, `KEYDOWN`/`KEYUP` — keyboard for chord-driven game controls and text entry
- `EXEC`, `WAIT`, `SLEEP` — launch the rebuilt game executable, wait for it to exit, time-gate retries
- `READ`, `WRITE` — pull crash dumps and build logs back; push patched config files

The "remote hands" generalisation came after — once the agent was in place for the port, the author recognised the same surface was directly useful for any AI-driven Windows automation, restructured the source into `agents/windows-nt/` with a formal PROTOCOL.md, dropped the game-specific framing, started a sibling `agents/windows-modern/` for current Windows, and the project became Agent Remote Hands.

Knowing this matters for two reasons: it explains why the verb surface is the shape it is (game-automation surface, born from VNC frustration — the bias toward fine-grained input fidelity and the file/process verbs reflect specific limits of what came before), and it sets the lineage record straight (the project's seed was a concrete user-driven need, not abstract tool-building).

## Two-commit history preserved here

The seed tag captures `vm_agent` at the two commit states it had upstream, with provenance preserved in commit messages. The 90-minute gap between them is genuine: feature 2 was bug-driven evolution found in real use within an hour and a half of feature 1 landing.

### Commit 1 — `Add tools/vm_agent: TCP control agent for the build VM`

*Captures the upstream initial commit (27 April 2026, 02:33 BST).*

Initial cut. 636-line `agent.c`, single-file C with no dependencies beyond the Win32 SDK. Builds with VC6's `cl.exe` via `build.bat` to a ~50 KB statically-linked PE32 i386 console executable. Companion `vmctl` Python script provides the host-side wrapper (parses responses, converts BMP screenshots to PNG via macOS `sips`). `README.md` documents setup and the protocol's text-line format with length-prefixed binary for `SHOT`/`READ`/`WRITE`.

### Commit 2 — `vm_agent: cursor overlay, WINFIND/WINMOVE, hot-swap support`

*Captures the upstream feature commit (27 April 2026, 03:59 BST — 90 minutes after Commit 1).*

Three real-world bug-fix-driven additions found in the first hour and a half of using the agent against the rebuilt game:

- **Cursor overlay in screenshots.** `BitBlt` doesn't capture the OS hardware cursor — Claude couldn't see where it was clicking. Fix: `GetCursorInfo` + `DrawIcon`, both dynamically resolved from `user32.dll` because VC6's stock SDK predates the `CURSORINFO` struct definition.
- **`WINFIND` / `WINMOVE` verbs.** The game creates a 640×480 window at an arbitrary position. Aligning it at desktop `(0, 0)` makes screen-coords map 1:1 to game-coords, simplifying every subsequent click coordinate. `WINFIND <title-prefix>` returns `OK x y w h`; `WINMOVE x y <title-prefix>` repositions and brings foreground.
- **`vmctl` latin-1 encoding.** The German game-source path contained `ü`. Python's default UTF-8 turned `ü` into invalid CP1252 on the wire, breaking `xcopy` of the source dir into the VM. Fix: encode commands as latin-1 so single-byte `0xFC` survives to `CreateProcessA`.

Plus a new `build_new.bat` enabling a **hot-swap pattern**: the running `vm_agent.exe` can update itself without dropping the connection. Build to `vm_agent_new.exe` (new filename so the running `vm_agent.exe` stays unlocked), launch on alt port `8766`, dismiss Windows XP's first-run Security Alert via `Alt+U` (the `vmctl` driving the swap sends the keystroke), `taskkill` the old, copy `_new.exe` over the original, relaunch. Recipe lived in the upstream project's hand-off documentation.

Both commits in this tag preserve the original commit messages, authorship (William Isted), and Claude-co-authorship attribution.

## Provenance

- `agent.c`, `build.bat`, `build_new.bat`, `vmctl`, `README.md` extracted from the upstream port project's `tools/vm_agent/` directory at the two commit states described above.
- `vm_agent.exe` built locally by the author on the Windows XP VM with VC6 against `agent.c` per `build.bat`. PE32 i386 subsystem 4.00, 64 KB. Attached to the [`seed` GitHub pre-release](https://github.com/WilliamIsted/agent-remote-hands/releases/tag/seed), not committed to the tag tree (binaries don't belong in git history).
- `LICENSE` is the project's current Apache 2.0, applied retroactively at archive time. `vm_agent` originally shipped without an explicit licence file; its presence here doesn't reflect the original artefact's licensing state.
- `ORIGINS.md` (this file) is new at archive time and describes the artefact retrospectively; it didn't exist at either of the two original commits.

## What changed in the V1 extraction

The leap from this seed to V1's `agents/windows-nt/agent.c` (preserved at [`v0.1.0`](https://github.com/WilliamIsted/agent-remote-hands/releases/tag/v0.1.0)) was substantial:

| Field | Pre-V1 (`vm_agent`) | V1 (`agents/windows-nt/agent.c`) |
|---|---|---|
| Filename | `agent.c` (in `tools/vm_agent/`) | `agent.c` (in `agents/windows-nt/`) |
| Header framing | "Minimal control agent for the Windows XP build VM" | "Agent Remote Hands, windows-nt target" |
| Env var | `VM_AGENT_PORT` | `REMOTE_HANDS_PORT` (with `VM_AGENT_PORT` back-compat) |
| Binary name | `vm_agent.exe` | `remote-hands.exe` |
| Spec reference | None — verb list lived inline as C comments and in this archive's `README.md` | `PROTOCOL.md` formalised at repo root |
| Verb shape | `SHOT`, `MPOS`, `KEY`, `MOVEREL`, `WHEEL`, `WINFIND`, `WINMOVE`, `EXEC`, … | V1 spec verbs (`PING`, `CAPS`, `INFO`, `SHOT`, `RUN`, `LIST`, `WINLIST`, …) |
| Build flags | `/O2` (optimise for speed) | `/O1 /Os` (Tier-1 size optimisation) |
| Linker | Default invocation, `/SUBSYSTEM:CONSOLE` (no version) | `/OPT:REF /OPT:ICF /MERGE:.rdata=.text /SUBSYSTEM:CONSOLE,4.00` (NT4 explicit) |
| OS reach | Implicit (XP-targeted; loads on whatever the VC6 default subsystem accepts) | Explicit NT4 → Server 2003 support matrix in COMPATIBILITY.md |
| Service mode | Not present | `--install` / `--uninstall` SCM registration (paper-only — V1 NT source never compiled successfully; see footnote) |
| Hot-swap recipe | First-class (`build_new.bat` + alt-port + `Alt+U` + taskkill cycle) | Dropped — V1 NT was never built, so no hot-swap testing happened |

## A footnote on V1 NT

V1 attempted to formalise this seed into `agents/windows-nt/agent.c` with GDI+ screen capture, modern Win32 idioms, and an `--install` SCM mode. The source as committed to V1's `main_ORIG` never compiled successfully on VC6 — the build log preserved with the V1 release shows roughly 50 errors rooted in SDK-version mismatches (GDI+ types, `ULONG_PTR`, wide-string literals, `InterlockedIncrement` signature). Fixing them was non-trivial; rather than push through, the project pivoted to V2 (which dropped windows-nt from active scope and focused on windows-modern). That makes `vm_agent` — preserved here — the only working NT-era binary in the lineage.

## Reading the seed

```bash
git checkout seed                # state-2: cursor overlay + WINFIND/WINMOVE + hot-swap
git checkout seed^               # state-1: initial cut
git log seed --oneline           # see both commits with original commit messages
git diff seed^ seed -- agent.c  # see the +106 lines that landed in commit 2
```

The binary is attached to the [`seed` GitHub pre-release](https://github.com/WilliamIsted/agent-remote-hands/releases/tag/seed), not committed to the tag tree.

## Why preserve this

Three reasons:

1. **Lineage record.** Anyone investigating "where did this project start?" can find the actual seed — and its 90-minute initial-evolution arc — without git-archaeology guessing across repos.
2. **Honest framing of V1.** Calling V1 "the first release" is more honest if the pre-V1 seed is preserved separately as the actual first thing — V1 then becomes "the first formally-structured release that pivoted before stabilising" rather than "the start".
3. **Shows the deltas that justified V1's structure.** Comparing this seed (and its bug-driven evolution) with V1's `agents/windows-nt/agent.c` makes the value of the V1 extraction (formal protocol, size optimisations, service installer attempt, multi-OS support matrix) concrete — and shows where V1 over-reached relative to a working seed.

## Tag policy

This tag is write-once. Errata or clarifications go into the V1 archive's documentation or this project's commit history, not retroactively into the seed.
