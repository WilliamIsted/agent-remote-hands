# windows-modern agent — build notes

## Toolchain

CMake + modern MSVC. Tested with Visual Studio 2019 / 2022 Build Tools and Windows 10/11 SDK. No third-party dependencies.

```bash
# From the agents/windows-modern/ directory
cmake -B build -A x64
cmake --build build --config Release
# Output: build/Release/remote-hands.exe
```

For x86 (32-bit) build: `cmake -B build -A Win32`. The agent runs on either architecture; x64 is the default for modern targets.

## What the build produces

A single statically-linked `remote-hands.exe`, ~50–80 KB at Release. The manifest (`manifest.xml`) is embedded by the linker via `/MANIFEST:EMBED`, so the binary advertises its DPI / long-path / UTF-8 ACP / `supportedOS` to Windows at load time.

No external runtime dependency beyond the OS-supplied DLLs (`kernel32`, `user32`, `gdi32`, `advapi32`, `shell32`, `ole32`, `oleaut32`, `taskschd`, `ws2_32`).

## Why CMake and not a `build.bat`

The windows-nt agent uses a single-file `build.bat` because it only ever needs VC6. windows-modern needs:

- The manifest embedded
- Multiple OS-supplied libs linked
- Optional toolset switching (VS 2019 vs 2022, x86 vs x64)
- Future `agents/windows-modern/` extensions (Windows.Graphics.Capture, which wants C++/WinRT headers)

CMake is the smallest tool that handles all of those without growing into hand-edited project files. If the build complexity ever inverts and CMake is overkill, drop back to a `build.bat` — the source is single-file and portable.

## Dependencies on the build host

- Visual Studio 2019 Build Tools or later (free download), with the **Desktop development with C++** workload.
- Windows 10 SDK 10.0.19041 or later (for the `taskschd.h` Task Scheduler 2.0 headers).
- CMake 3.20+.

The build host's Windows version doesn't matter — Windows 10/11 dev box is fine, the produced binary will load on Vista+.

## Cross-compile

Possible with MinGW-w64, but the Task Scheduler 2.0 COM headers don't ship cleanly with MinGW; you'd need to either skip `--install` (foreground-only build) or hand-import the COM interfaces from `taskschd.idl`. For now, build on Windows.

## Size optimisations

Same Tier-1 flags as windows-nt are already in `CMakeLists.txt` for Release: `/O2 /Os /Gy /GS-` plus `/OPT:REF /OPT:ICF`. Tier 2 (drop the CRT) is harder here because Task Scheduler interaction uses C++ exception unwinding through COM — pulling all of that out is more invasive than it was for the pure-C VC6 build. **Stop at Tier 1.**

## Known gotcha — manifest `trustInfo` vs CMake's `/MANIFESTUAC`

`CMakeLists.txt` passes both `/MANIFESTUAC:level='asInvoker' uiAccess='false'` and `/MANIFESTINPUT:manifest.xml` to the linker. The `/MANIFESTUAC` flag synthesises a `<trustInfo>` block; if `manifest.xml` *also* contains a `<trustInfo>` block, `mt.exe` chokes with the unhelpful error:

```
manifest.xml : general error c1010070: Failed to load and parse the manifest.
Windows was unable to parse the requested XML data.
LINK : fatal error LNK1327: failure during running mt.exe
```

The error message points at "XML parse failure", but the actual XML is well-formed — the merge step is what fails. Fix: keep `<trustInfo>` out of `manifest.xml` and let `/MANIFESTUAC` own it.

## What's still TODO

The scaffold is functionally complete except for one item:

- **Windows.Graphics.Capture (WGC) for `SHOT` / `SHOTRECT` / `SHOTWIN`.** Currently uses BitBlt across the virtual screen, which captures all monitors at correct DPI but renders hardware-accelerated and DRM-protected surfaces as black. The WGC switch is one new file (`capture_wgc.cpp` using C++/WinRT) plus a fallback selector. The BitBlt path stays as the ≤Win10-1809 fallback. Marked at the `capture_bmp_rect` definition in `agent.cpp`.

Everything else — `SendInput` for input, virtual-screen multi-monitor, DPI awareness, Task Scheduler logon-task install, INFO advertising the modern flags — is in place and exercised by the conformance suite.

## Running

```bash
# Foreground, default port 8765
build/Release/remote-hands.exe

# Different port
build/Release/remote-hands.exe 9999
# or: set REMOTE_HANDS_PORT=9999

# Install as a logon task (run elevated once on the target box)
build/Release/remote-hands.exe --install

# Remove
build/Release/remote-hands.exe --uninstall

# Enable destructive verbs (LOGOFF/REBOOT/SHUTDOWN) — opt-in per process
set REMOTE_HANDS_POWER=1
build/Release/remote-hands.exe
```
