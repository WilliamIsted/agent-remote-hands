# Verb catalogue

A scannable conceptual index of every verb the protocol defines. One line per verb, organised by namespace. Read this when you need to know *what verbs exist and what they do*; read [`PROTOCOL.md`](PROTOCOL.md) when you need full signatures, error codes, or framing details; read [`LLM-OPERATORS.md`](LLM-OPERATORS.md) when you're an LLM driving an agent.

Tier shorthand: **R** = read, **C** = create, **U** = update, **D** = delete, **X** = extra_risky, **—** = lifecycle (any tier). The required tier matches each verb's CRUDX letter; the ladder is strict (read < create < update < delete < extra_risky).

Format per line:

```
`verb.name` (tier): One-line purpose. Flags: --flag (purpose), --flag (purpose).
```

Verbs without optional flags omit the `Flags:` clause. Mandatory positional arguments are described in the purpose, not as flags.

Today's verb namespace is Windows-flavoured; some namespaces are intrinsically Windows-shaped (HWND, UIA, registry) and tagged inline as *(Windows-specific)*. Other-OS implementations of agnostic-concept namespaces (`screen`, `input`, `file`, `process`, `clipboard`, `watch`, parts of `system`) are admitted by the protocol — see [`PROTOCOL.md`](PROTOCOL.md) §1 for the platform-extensibility note.

---

## `system.*`

Identity, health, and lifecycle.

- `system.info` (R): Returns agent identity, OS, hostname, integrity level, monitor inventory, image-format support, and capability summary.
- `system.capabilities` (R): Returns the verb→required-tier map exhaustively for the current build.
- `system.health` (R): Liveness check; returns `OK` if the agent is responsive.
- `system.shutdown_blockers` (R): Lists windows that have called `ShutdownBlockReasonCreate`. *(Windows-specific.)*
- `system.lock` (R): Locks the workstation (`LockWorkStation`); `not_supported` on platforms without lock support.
- `system.reboot` (X): Initiates an OS reboot. Flags: --delay (defer N seconds in-process before issuing), --force (override blockers), --reason (Win32 shutdown reason code).
- `system.shutdown` (X): Initiates an OS shutdown. Flags: --delay, --force, --reason (as `system.reboot`).
- `system.logoff` (X): Logs the current user off. Flags: --force.
- `system.hibernate` (X): Hibernates the machine.
- `system.sleep` (X): Suspends the machine to sleep.
- `system.power.cancel` (X): Aborts an in-process delayed shutdown scheduled via `--delay`. Capability-gated.

## `screen.*`

Pixel capture.

- `screen.capture` (R): Captures the desktop, a region, a single window, or a single monitor. Flags: --region (x,y,w,h sub-rectangle), --window (capture a specific HWND), --monitor (Nth physical monitor, 0-based), --format (png / webp / webp:Q / bmp). `--region` / `--window` / `--monitor` are mutually exclusive; if none, captures the entire virtual screen.

## `window.*` *(Windows-specific — uses HWND identifiers)*

Top-level window enumeration and control.

- `window.list` (R): Enumerates top-level windows with bounds, title, owning PID, and monitor index. Flags: --filter (title-pattern restriction), --all (include invisible windows).
- `window.find` (R): Returns the first top-level window whose title matches the given pattern.
- `window.focus` (U): Brings the given HWND to the foreground; surfaces foreground-lock denial as `ERR lock_held`.
- `window.close` (U): Sends `WM_CLOSE` to the given HWND; the window may decline.
- `window.move` (U): Moves and resizes the given HWND to the supplied bounds.
- `window.state` (R): Reports whether the window is minimised / maximised / normal / hidden.

## `input.*`

Synthetic mouse and keyboard input. Cross-IL targets surface as `ERR uipi_blocked`. Keyboard verbs do *not* reach RawInput / DirectInput targets (see [`PROTOCOL.md`](PROTOCOL.md) §4.4).

- `input.click` (U): Synthesises a mouse click at the given screen coordinates. Flags: --button (left / right / middle; default left).
- `input.move` (U): Moves the cursor to the given coordinates without clicking.
- `input.scroll` (U): Sends mouse-wheel notches at the given coordinates (positive = up).
- `input.key` (U): Presses a virtual key by name (`enter`, `F4`, `a`, …). Flags: --modifiers (comma-separated, e.g. `ctrl,shift`).
- `input.type` (U): Types a UTF-8 string from a length-prefixed payload; handles Unicode and quote-escape hazards.
- `input.send_message` (U): Synchronous `SendMessage` escape hatch with HWND, msg, wparam, lparam. *(Windows-specific.)*
- `input.post_message` (U): Non-blocking `PostMessage` peer of `input.send_message`; use when the target's message pump is unresponsive. *(Windows-specific.)*

## `element.*` *(Windows-specific — UIA-based; uses `elt:` identifiers, connection-scoped)*

UI Automation introspection and control.

- `element.list` (R): Enumerates interactable / named elements visible on screen. Flags: --region (restrict enumeration to a sub-rectangle).
- `element.tree` (R): Recursive TreeWalker descent from the given element id, returning depth-tagged children.
- `element.at` (R): Hit-tests at screen coordinates and returns the element under the cursor.
- `element.find` (R): Finds the first element matching `<role> <name-pattern>`; distinguishes `not_found` from `uia_blind` (cross-IL barrier).
- `element.wait` (R): Polling form of `element.find` with a deadline; capability-gated.
- `element.find_invoke` (U): Compound `element.find` + `element.invoke` in one round-trip.
- `element.at_invoke` (U): Compound `element.at` + `element.invoke` in one round-trip.
- `element.invoke` (U): Invokes the InvokePattern on the given element id.
- `element.toggle` (U): Toggles a TogglePattern element; returns the new state (on / off / indeterminate).
- `element.expand` (U): Expands an ExpandCollapsePattern element; returns the new state.
- `element.collapse` (U): Collapses an ExpandCollapsePattern element; returns the new state.
- `element.focus` (U): Sets keyboard focus on the given element id.
- `element.text` (R): Reads text from the given element via TextPattern (preferred) or ValuePattern (fallback).
- `element.set_text` (U): Writes text to the given element from a length-prefixed payload; surfaces `readonly` / `not_supported_by_target` distinctly.

## `file.*`

File operations. UTF-8 paths. For directory-only verbs see `directory.*`.

- `file.read` (R): Reads an entire file and returns its bytes as the response payload.
- `file.write` (U): Writes a length-prefixed payload as the entire file contents, truncating any existing file.
- `file.write_at` (U): Random-access write at a given byte offset; the chunked-upload primitive used by the MCP bridge for large files. Flags: --truncate (only meaningful at offset 0; clears the file first).
- `file.stat` (R): Stat-like response for a single path; tolerates files and directories.
- `file.delete` (D): Deletes a file or empty directory. For non-empty directories see `directory.delete` (planned) or use a recursive walk.
- `file.exists` (R): Reports whether a path exists and, if so, its type. Tolerates files and directories.
- `file.wait` (R): Resolves when a path matching the glob appears, or returns `ERR timeout`. Tolerates files and directories.
- `file.rename` (U): Moves or renames a file or directory.

## `directory.*`

Directory operations. UTF-8 paths. *(New namespace in v2.1 — split out from `file.*`.)*

- `directory.list` (R): Lists directory entries with type, size, and mtime. *(Renamed from `file.list` in v2.1.)*
- `directory.stat` (R): Reports entry count and mtime for a directory. Returns `not_a_directory` if the path is a file.
- `directory.exists` (R): Reports whether a path exists and is a directory. Returns false for missing paths and for files.
- `directory.create` (C): Creates a directory. Flags: --parents (mkdir -p semantics). *(Renamed from `file.mkdir` in v2.1.)*
- `directory.rename` (U): Renames or moves a directory; the same primitive handles both. Flags: --overwrite (replace `<dst>` if it exists), --cross-fs (allow copy+rmtree fallback across filesystems).
- `directory.remove` (D): Removes a directory. Flags: --recursive (`rm -rf` — recursively removes contents first). Reparse points / symlinks are not traversed.

## `process.*`

Process management.

- `process.list` (R): Enumerates running processes with pid, image path, and ppid. Flags: --filter (image-name pattern).
- `process.start` (C): Spawns a process via `CreateProcess`, returning its pid. Flags: --stdin (length-prefixed bytes piped to the child's stdin).
- `process.shell` (C): Spawns via `ShellExecuteEx`; handles paths with spaces / unicode without shell-escape hazards. Flags: --args (parameter string passed verbatim), --verb (non-default ShellExecute verb, e.g. `runas`, `print`, `edit`). *(Windows-specific.)*
- `process.kill` (D): Terminates the given pid (`TerminateProcess`).
- `process.wait` (R): Waits for the given pid to exit, with a deadline; returns the exit code or `ERR timeout`.

## `registry.*` *(Windows-specific — uses `HKLM\…` / `HKCU\…` paths)*

Windows registry operations.

- `registry.read` (R): Reads a registry key or single value. Flags: --value (read one named value instead of the whole key).
- `registry.write` (U): Writes a typed registry value (REG_SZ, REG_DWORD, etc.) at the given key+name.
- `registry.delete` (D): Deletes a value or the whole key. Flags: --value (delete one named value instead of the whole key).
- `registry.wait` (R): Waits for a key to change (`RegNotifyChangeKeyValue`), with a deadline; returns `ERR timeout` on expiry.

## `clipboard.*`

Clipboard text I/O.

- `clipboard.get` (R): Reads the clipboard's text contents as UTF-8; empty payload if no text. *(Renamed from `clipboard.read` in v2.1.)*
- `clipboard.set` (U): Replaces the clipboard's text contents with a length-prefixed UTF-8 payload. *(Renamed from `clipboard.write` in v2.1.)*

## `watch.*`

Subscription-based observation. Each verb returns a `subscription_id`; events arrive as out-of-band `EVENT` frames until cancelled or auto-cancelled (see [`PROTOCOL.md`](PROTOCOL.md) §6).

- `watch.region` (R): Streams image frames of a screen region. Flags: --interval (poll period in ms), --until-change (auto-cancel after the first changed frame).
- `watch.process` (R): Emits one event when the given pid exits, then auto-cancels.
- `watch.window` (R): Emits events on window appearance / disappearance. Flags: --title-prefix (only fire for windows whose title starts with the pattern).
- `watch.element` (R): Emits one event when the given element id is invalidated, then auto-cancels.
- `watch.file` (R): Emits events on file create / modify / delete matching a glob (`ReadDirectoryChangesW`-based).
- `watch.registry` (R): Emits events on registry-key changes (`RegNotifyChangeKeyValue`-based). *(Windows-specific.)*
- `watch.cancel` (R): Ends a subscription by id; idempotent on already-cancelled ids.

## `connection.*`

Lifecycle and tier negotiation. See [`PROTOCOL.md`](PROTOCOL.md) §2 for the state-machine details.

- `connection.hello` (—): First verb after connect; takes client name and protocol version; gates further verbs.
- `connection.tier_raise` (—): Raises the connection to a higher ladder rung (`create` / `update` / `delete` / `extra_risky`); requires a token read from the agent's token file.
- `connection.tier_drop` (—): Voluntarily drops the connection to a lower tier; requires no token.
- `connection.reset` (—): Flushes wire-format state; the recovery primitive after `ERR wire_desync`.
- `connection.close` (—): Drains pending EVENT frames and closes the connection cleanly.
