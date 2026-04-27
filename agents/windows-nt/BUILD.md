# windows-nt agent — build notes

## Toolchain choices

The toolchain that compiles the binary and the OS that runs it are **independent**. What matters is:

1. The **PE subsystem version** in the linker flags (governs which Windows versions can load the binary).
2. The **APIs called** (must exist on the oldest target — gated by `_WIN32_WINNT` define and runtime `GetProcAddress` checks).
3. The **CRT linkage** — `/MT` (used by `build.bat`) statically embeds the C runtime so no `msvcrt.dll`/`msvcr*.dll` is required on the target.

None of those care which Windows version compiled the binary. They care which **toolchain** and which **headers / libs** were targeted. Three working paths:

### 1. Modern MSVC (Visual Studio 2017 / 2019 / 2022)

- Use `cl.exe` from the Build Tools, target x86, `/D_WIN32_WINNT=0x0500`, `/SUBSYSTEM:CONSOLE,5.00` (or `,4.00` for NT4 compatibility).
- The Windows 10/11 SDK still has the legacy function declarations behind `_WIN32_WINNT` — `keybd_event`, `BitBlt`, etc. — so the source compiles unchanged.
- **Caveat:** modern `cl.exe` defaults to MSVCRT versions that exist only on newer Windows. The fix is the **XP-targeting toolset** that VS 2017 still ships (`v141_xp`), which produces XP-compatible binaries from a modern dev box. VS 2019+ requires installing the v141_xp component manually.

### 2. MinGW-w64 cross-compile (from Linux / macOS)

```sh
i686-w64-mingw32-gcc -O2 -static -D_WIN32_WINNT=0x0400 \
    -Wl,--subsystem,console:4.00 \
    agent.c \
    -lws2_32 -luser32 -lgdi32 -ladvapi32 \
    -o remote-hands.exe
```

- Single static .exe, runs from NT4 onwards. No Windows machine needed.
- Source-code wrinkle: MinGW's `_snprintf` differs slightly from MSVC's. The current source uses `_snprintf` which exists in both, but a clean MinGW build may want `__MINGW_USE_VC2005_COMPAT` defined or a switch to `snprintf`.

### 3. VC6 on XP (period-correct path)

What `build.bat` currently assumes. Smaller binaries (~20 KB after Tier-1 size flags), no surprise CRT dependencies, output runs from NT4 onwards. Useful if you're already running an XP VM for testing — the same VM is the build host.

## Size optimisation tiers

The current `build.bat` applies **Tier 1**. The other tiers exist as options if size pressure ever justifies the complexity cost.

### Tier 1 — flag tuning (~50 KB → ~20 KB)

What `build.bat` does today:

```
cl /nologo /MT /O1 /Os /Gy /GS- /W3 /D_WIN32_WINNT=0x0500 ^
   agent.c ^
   /link wsock32.lib user32.lib gdi32.lib advapi32.lib ^
   /OPT:REF /OPT:ICF /MERGE:.rdata=.text ^
   /OUT:remote-hands.exe /SUBSYSTEM:CONSOLE,4.00
```

- `/O1 /Os` — optimise for size, not speed (agent is I/O-bound).
- `/Gy` + `/OPT:REF` + `/OPT:ICF` — function-level COMDATs, drop unreferenced, fold identical (mouse `do_mdown`/`do_mup` fold; several `WIN*` handlers fold).
- `/GS-` — disable stack-cookie buffer-overrun checks (~30 bytes per function on modern MSVC; moot on VC6).
- `/MERGE:.rdata=.text` — collapse read-only data into the code section, kill alignment slop.
- `/SUBSYSTEM:CONSOLE,4.00` — PE subsystem 4.00, loadable from NT 4.0 onwards.

**No source changes, no behavioural change.** The right stopping point for an ordinary deployment.

### Tier 2 — drop the CRT (~20 KB → 6–8 KB)

The remaining bulk is C runtime startup and `printf` / `malloc` family. To eliminate:

- `/NODEFAULTLIB`, link only `kernel32.lib`, `user32.lib`, `gdi32.lib`, `advapi32.lib`, `wsock32.lib`.
- Provide a custom `mainCRTStartup` (parses `GetCommandLineA` if argv is needed, or skips it).
- Replace `malloc` / `free` with `HeapAlloc(GetProcessHeap(), …)` / `HeapFree`.
- Replace `_snprintf` with `wsprintfA` (in user32, already linked). Caveat: `wsprintf` lacks `%I64u`, so the `LIST` / `STAT` 64-bit-size formatter needs ~20 lines of hand-rolled int64-to-decimal.
- Replace `printf` / `fprintf` console writes with `WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), …)`.
- Replace `strcmp` / `strlen` / `memcpy` either with hand-rolled versions or with kernel32's `lstrcmpA` / `lstrlenA`. Or use `#pragma intrinsic`.
- Replace `atol` / `atoi` with a 5-line decimal parser.

Result: **6–8 KB**, no CRT, single-file source, no new DLL deps.

### Tier 3 — PE-header tricks (~6–8 KB → ~3–4 KB)

The MSVC linker's PE header is wasteful. Demoscene-grade options:

- **Crinkler** (compressing linker, replaces `link.exe`, packs sections with arithmetic coding). Drops a 6 KB clean Win32 binary to ~2 KB.
- **UPX compression** post-build — `upx --best --lzma remote-hands.exe`. ~6 KB → ~3 KB.
- **Manual PE editing** to merge `.text` / `.data` / `.rdata` and reduce section alignment from 4096 to 512. Saves ~3 KB.

Returns shrink fast and AV false-positive risk grows fast (compressed Win32 binaries trip many heuristics). **Don't go here unless you specifically need to fit on era-appropriate media** (3.5" floppies, 1.44 MB).

## Deployment artefacts

After `build.bat`:

- `remote-hands.exe` — the only file needed on the target. Statically linked CRT + Win32 imports only.
- No companion DLLs, no manifests, no installer.

To deploy:
- Copy the .exe to the target.
- Either run in foreground (`remote-hands.exe`) or run elevated and `remote-hands.exe --install` to register as a service (NT 2000 / XP / 2003 only — see `COMPATIBILITY.md`).

To override port: `remote-hands.exe 9999` or set `REMOTE_HANDS_PORT=9999`.

To enable the gated power verbs (`LOGOFF`/`REBOOT`/`SHUTDOWN`): set `REMOTE_HANDS_POWER=1` before launch.
