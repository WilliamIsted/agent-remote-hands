# windows-modern agent

Modern Windows (10 / 11) target for Agent Remote Hands. Implements the v2 wire protocol — see [`PROTOCOL.md`](../../PROTOCOL.md) at the repo root for the canonical spec.

## Build

From a Developer PowerShell with CMake on PATH:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/remote-hands.exe`.

## Run

```powershell
.\build\Release\remote-hands.exe                 # Default: TCP 8765, no mDNS
.\build\Release\remote-hands.exe --discoverable  # Advertise via mDNS
.\build\Release\remote-hands.exe --port 9000     # Custom port
```

The agent generates a token at `%ProgramData%\AgentRemoteHands\token` on first run. Clients use this token to elevate from `observe` tier to `drive` or `power` via `connection.tier_raise`.

## Install (autostart)

Run as Administrator from the build output:

```powershell
.\remote-hands.exe --install
```

This copies the binary to `%ProgramFiles%\AgentRemoteHands\` and registers a Task Scheduler logon-task. The agent autostarts on user logon in the interactive desktop session so it can drive the visible UI.

`.\remote-hands.exe --uninstall` removes the task and binary.

## Debug

The agent logs to stderr and `OutputDebugString` (visible in DebugView). Use `--port` with a non-default value to avoid conflicting with an installed copy on the same machine.

## Source layout

- `src/main.cpp` — entry point, lifecycle
- `src/config.{hpp,cpp}` — env / CLI parsing
- `src/server.{hpp,cpp}` — TCP listener, accept loop
- `src/log.hpp` — logging helpers (header-only)
- `src/errors.hpp` — error code enum + wire serialisation
- `src/protocol.{hpp,cpp}` — wire-format reader / writer (added in build phase 2)
- `src/connection.{hpp,cpp}` — per-connection state machine (added in build phase 3)
- `src/verbs/*.cpp` — per-namespace verb handlers (added in build phases 5–14)

## Conformance

Run the conformance suite against a running agent:

```bash
python ../../tests/conformance/run.py <host> 8765
```

Tests gate on advertised capabilities — verbs the agent doesn't implement get the test skipped, not failed.

## License

Apache-2.0. See [`LICENSE`](../../LICENSE) at the repo root.
