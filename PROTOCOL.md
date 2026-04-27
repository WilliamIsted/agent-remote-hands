# Wire protocol

Version: **1**

Text-based, line-oriented, request/response over TCP. Every command is one line terminated by `\n` (`\r` is tolerated and ignored). Every response begins with `OK` or `ERR`. Commands that return data use the form:

```
OK <length>\n
<exactly length bytes of payload>
```

A few verbs prefix metadata fields between `OK` and `<length>` — e.g. `RUN` returns `OK <exit-code> <length>\n<bytes>`. Multi-row tabular responses (`LIST`, `PS`, `WINLIST`, `DRIVES`, `ENV` without arg) pack newline-separated rows into a single payload; fields within a row are separated by tab (`\t`).

A small number of verbs (`WATCH`, `WAITFOR`) emit *multiple* `OK <ts> <length>\n<bytes>` responses on a single command, terminated by a literal `END\n` line. These verbs hold the connection for the duration; clients that need to interleave commands open a second connection.

The connection stays open after each command; the client closes it (or sends `QUIT`) when done. The server stays running and accepts further connections.

## Encoding

- Text is **ANSI / Latin-1** on the wire (matches the Western-codepage Windows ANSI APIs the XP agent uses).
- Path arguments are passed verbatim to the platform's native API. On Windows this means `CreateProcessA` / `CreateFileA`; on Linux it would mean UTF-8 paths.

## Versioning and capabilities

Every agent must implement `PING` and `CAPS`. Capability discovery is how clients negotiate without breaking on older agents.

| Verb             | Response                          | Required |
|------------------|-----------------------------------|----------|
| `PING`           | `OK pong`                         | yes      |
| `CAPS`           | `OK <verb> <verb> ... <verb>`     | yes      |
| `INFO`           | `OK <key>=<value> ...`            | recommended (target os, arch, protocol version) |

If a verb is not in `CAPS`, the client should not send it; agents may also respond `ERR unsupported` to a known verb on a platform where it can't be implemented.

## Capability flags

Every agent SHOULD return the keys below from `INFO`, on a single line as `key=value` tokens separated by spaces, so clients (and the MCP server) can reason about capability shape without per-call probing. Booleans are `yes`/`no`, enums are short lowercase strings, limits are integers. Unknown keys are ignored; absent keys mean "unknown" — clients should assume the conservative default.

### Identity

| Key        | Type   | Example      | Notes |
|------------|--------|--------------|-------|
| `os`       | string | `windows-nt` | Target family/version. Required. |
| `arch`     | string | `x86`, `x64`, `arm64` | CPU architecture. |
| `protocol` | int    | `1`          | Wire protocol version. Required. |
| `user`     | string | `Admin`      | Account the agent is running as. |
| `hostname` | string | `WINXP-VM`   | Computer name. |

### Display

| Key              | Type   | Example | Notes |
|------------------|--------|---------|-------|
| `capture`        | string | `gdi`, `wgc`, `x11`, `quartz` | Screenshot mechanism. Implies which surfaces are visible — `gdi` may miss hardware-accelerated/secure surfaces; `wgc` handles them. |
| `multi_monitor`  | yes/no | `no`    | If `yes`, `SCREEN` reports the virtual-screen rect and `SHOT` spans all displays. |
| `dpi_aware`      | yes/no | `no`    | If `no`, coordinates are virtualised — on a HiDPI display, `MOVE`/`SHOT` operate on a scaled grid, not physical pixels. |
| `cursor_in_shot` | yes/no | `yes`   | Whether the OS cursor is composited into the screenshot. If `no`, clients must call `MPOS` to locate it. |

### Input

| Key     | Type   | Example | Notes |
|---------|--------|---------|-------|
| `input` | string | `legacy`, `sendinput`, `xtest`, `cgevent` | Injection backend. `legacy` = `keybd_event`/`mouse_event` (synthetic-vs-real distinguishable, ASCII-only). `sendinput` supports Unicode and scan-code injection. |

### Filesystem

| Key             | Type   | Example         | Notes |
|-----------------|--------|-----------------|-------|
| `path_encoding` | string | `ansi`, `utf8`  | How `READ`/`WRITE` path arguments are interpreted by the agent. The wire stays Latin-1 (see Encoding); this flag tells the client which byte sequences map to which on-disk paths. |
| `max_path`      | int    | `260`           | Longest path the agent will accept. |

### Windowing

| Key       | Type   | Example | Notes |
|-----------|--------|---------|-------|
| `windows` | yes/no | `yes`   | Whether the `WIN*` verbs are meaningful. `no` on headless sessions or platforms with no top-level window concept. |

### UI Automation

| Key             | Type   | Example | Notes |
|-----------------|--------|---------|-------|
| `ui_automation` | string | `uia`   | `uia` when the agent exposes the `ELEMENTS` / `ELEMENT_*` verbs via Windows UI Automation (windows-modern). `msaa` when the older Microsoft Active Accessibility surface is exposed (windows-nt — planned). `no` when no accessibility surface is available. Clients check this before sending element verbs; absent flag means "no". |

### Concurrency

| Key               | Type | Example | Notes |
|-------------------|------|---------|-------|
| `max_connections` | int  | `4`     | Max simultaneous client connections the agent will admit. The (N+1)th connection receives `ERR busy` and is closed immediately. Clients that need to interleave commands with a long-running verb (e.g. `WATCH`) open a second connection rather than queueing on a single one. |

### Image formats

| Key             | Type            | Example         | Notes |
|-----------------|-----------------|-----------------|-------|
| `image_formats` | comma-list      | `bmp,png,webp`  | Image encodings the agent's capture verbs (`SHOT`, `SHOTRECT`, `SHOTWIN`, `WATCH`, `WAITFOR`) accept. `bmp` is always present (cheap, raw, large). `png` indicates GDI+ availability — present on Windows XP+ and Vista+ targets when `gdiplus.dll` loads. `webp` indicates a bundled libwebp encoder; lossy quality is selected per-call via the `webp:NN` token. Clients pick a format by prefixing the capture verb's args; absent prefix defaults to `bmp` for back-compat. |

### Power

| Key     | Type   | Example | Notes |
|---------|--------|---------|-------|
| `power` | yes/no | `no`    | If `yes`, the agent exposes `LOGOFF` / `REBOOT` / `SHUTDOWN`. Opt-in per deployment — most agents should default to `no`. |

### Discovery

| Key         | Type   | Example | Notes |
|-------------|--------|---------|-------|
| `discovery` | string | `mdns`  | `mdns` when the agent is broadcasting itself via mDNS / DNS-SD on the LAN (service type `_remote-hands._tcp.local.`). `no` otherwise. **Opt-in** per deployment via `REMOTE_HANDS_DISCOVERABLE=1` or `--discoverable` — the protocol has no auth, so advertising on an untrusted network is a footgun. Clients can find advertised agents with the bundled `client/hostctl-discover` script or any DNS-SD browser. |

### Example

```
> INFO
< OK os=windows-nt arch=x86 protocol=1 capture=gdi multi_monitor=no dpi_aware=no cursor_in_shot=yes input=legacy path_encoding=ansi max_path=260 windows=yes user=Admin hostname=WINXP-VM max_connections=4 power=no
```

## Connection control

| Verb                           | Response               | Notes |
|--------------------------------|------------------------|-------|
| `PING`                         | `OK pong`              | Liveness check. |
| `QUIT` / `EXIT` / `BYE`        | `OK` then close        | Graceful disconnect. |
| `ABORT`                        | `OK`                   | Cancels every in-flight long-running verb (`RUN`, `SLEEP`, `WATCH`, `WAITFOR`, blocking `WAIT`) on every connection. Aborted verbs return `ERR aborted` to their respective clients; the connections themselves stay open and accept new commands. Send `ABORT` from a fresh side connection so it doesn't queue behind the verb you're cancelling. |

## Screen / cursor

The capture verbs all accept an optional leading format token. Available formats are advertised via the `image_formats` capability flag in `INFO`. The leading position is required for `SHOTWIN`/`WATCH`/`WAITFOR` (their trailing args are ambiguous with a format suffix); kept consistent across all five verbs.

| Token       | Meaning                                | Notes |
|-------------|----------------------------------------|-------|
| `bmp`       | Uncompressed 24-bit BMP                | Default. Always supported. ~6 MB for 1920×1080. |
| `png`       | Lossless PNG                           | Available where `image_formats` includes `png`. ~5–30× smaller than BMP. Encoder cost ~50–150 ms per frame on modern hardware. |
| `webp`      | Lossless WebP                          | Available where `image_formats` includes `webp`. ~25–35% smaller than PNG. |
| `webp:NN`   | Lossy WebP at quality NN (0–100)       | Available where `image_formats` includes `webp`. ~5–20× smaller than PNG. Quality 85 is visually lossless for screen content; quality 60–70 is the sweet spot for streaming. |

| Verb                                          | Response                       | Notes |
|-----------------------------------------------|--------------------------------|-------|
| `SCREEN`                                      | `OK <width> <height>`          | Primary display. |
| `MPOS`                                        | `OK <x> <y>`                   | Current cursor position in screen coords. |
| `SHOT [bmp\|png]`                             | `OK <length>\n<bytes>`         | Whole primary display (or virtual screen on multi-monitor agents). |
| `SHOTRECT [bmp\|png] <x> <y> <w> <h>`         | `OK <length>\n<bytes>`         | Region. Saves bandwidth on tight loops. |
| `SHOTWIN [bmp\|png] <title-prefix>`           | `OK <length>\n<bytes>`         | Region covering the matched top-level window. |

## Live capture

Both verbs use the multi-frame framing extension — multiple `OK <ts_ms> <length>\n<bytes>` responses per command, terminated by a literal `END\n`. They block the connection for their duration; to interleave commands, open a second connection (`max_connections` advertised in `INFO`). Both abort on `ABORT` from another connection or on socket disconnect.

**Change detection**: the agent captures a 32×32 thumbnail of the screen and compares it to the previous frame; a frame counts as "changed" when more than `<threshold_pct>` of pixels differ by more than 8/255 in any colour channel (default `1.0`). The thumbnail is captured *without* the cursor, so cursor movement alone never triggers a frame.

| Verb                                                              | Response                                                     | Notes |
|-------------------------------------------------------------------|--------------------------------------------------------------|-------|
| `WATCH [bmp\|png] <interval_ms> <duration_ms> [<threshold_pct>]`  | stream of `OK <ts_ms> <length>\n<bytes>`; ends with `END\n`     | Captures every `interval_ms` for `duration_ms`. The first frame is always sent as a baseline; subsequent frames sent only on detected change. `<ts_ms>` is milliseconds since the WATCH started. Output format defaults to `bmp` (encoder-free, but bandwidth-heavy); use `png` when bandwidth matters more than CPU. |
| `WAITFOR [bmp\|png] <timeout_ms> [<threshold_pct>]`               | `OK <ts_ms> <length>\n<bytes>` or `ERR timeout`                 | Polls internally (~50 ms) until the screen changes, returns one frame, ends. |

## Mouse

| Verb                           | Response   | Notes |
|--------------------------------|------------|-------|
| `MOVE <x> <y>`                 | `OK`       | Instant move. |
| `MOVEREL <dx> <dy>`            | `OK`       | Relative to current pos. |
| `CLICK [left\|right\|middle\|N]` | `OK`     | At current pos. Default left. |
| `DCLICK [button]`              | `OK`       | Double-click. |
| `MDOWN [button]`               | `OK`       | Press and hold (for drag). |
| `MUP [button]`                 | `OK`       | Release a held button. |
| `DRAG <x> <y> [button]`        | `OK`       | Press at current, move to (x,y), release. |
| `WHEEL <delta>`                | `OK`       | 120 per notch (positive = up). |

## Keyboard

| Verb                           | Response   | Notes |
|--------------------------------|------------|-------|
| `KEY <combo>`                  | `OK`       | e.g. `enter`, `alt-F4`, `ctrl-shift-s`. |
| `KEYS <text>`                  | `OK`       | Type literal text (one line). |
| `KEYDOWN <key>`                | `OK`       | Press and hold (for chords with mouse). |
| `KEYUP <key>`                  | `OK`       | Release a held key. |

Key names: single letters / digits, `F1`–`F24`, `enter`, `tab`, `esc`, `space`, `backspace`, `delete`, `insert`, `home`, `end`, `pgup`, `pgdn`, `up`, `down`, `left`, `right`, `win`, `ctrl`, `alt`, `shift`. Combos are `mod-mod-key`, separator `-` or `+`.

## Files

| Verb                                 | Response                          | Notes |
|--------------------------------------|-----------------------------------|-------|
| `READ <path>`                        | `OK <length>\n<bytes>`            | Whole-file read. |
| `WRITE <path> <length>\n<bytes>`     | `OK`                              | Whole-file write; creates or replaces. |
| `LIST <path>`                        | `OK <length>\n<rows>`             | One row per entry: `<type>\t<size>\t<mtime>\t<name>`. Type ∈ `F` (file), `D` (dir), `L` (link/reparse), `?` (other). `mtime` is Unix epoch seconds. `.` and `..` are omitted. |
| `STAT <path>`                        | `OK <type> <size> <mtime>`        | Same field meanings as `LIST`; single line. |
| `DELETE <path>`                      | `OK`                              | Deletes a file or empty directory. |
| `MKDIR <path>`                       | `OK`                              | Creates one directory level — parent must exist. |
| `RENAME <src><sep><dst>`             | `OK`                              | Moves or renames. `<sep>` is a tab if either path contains spaces, otherwise space. |

## Process

| Verb                                 | Response                                  | Notes |
|--------------------------------------|-------------------------------------------|-------|
| `EXEC <cmdline>`                     | `OK <pid>`                                | Async, no output capture. Use `WAIT` for the exit code. |
| `RUN <cmdline>`                      | `OK <exit> <length>\n<bytes>`             | Synchronous. Captures combined stdout+stderr. Blocks the agent connection until the child exits. |
| `WAIT <pid> [<timeout_ms>]`          | `OK <exit_code>` or `ERR timeout`         | Waits for an `EXEC`'d process. |
| `KILL <pid>`                         | `OK`                                      | `TerminateProcess` (or platform equivalent). |
| `PS`                                 | `OK <length>\n<rows>`                     | One row per process: `<pid>\t<image-name>`. |
| `SLEEP <ms>`                         | `OK`                                      | Sleeps the agent thread. |

## Clipboard

| Verb                                 | Response                          | Notes |
|--------------------------------------|-----------------------------------|-------|
| `CLIPGET`                            | `OK <length>\n<bytes>`            | Plain text only. Empty payload if the clipboard has no text. |
| `CLIPSET <length>\n<bytes>`          | `OK`                              | Replaces clipboard contents with the given text. |

## Windows (top-level windows on the target)

Optional; agents may omit on platforms where it doesn't apply (`windows=no` in `INFO`). All title arguments match by case-insensitive prefix on the visible top-level window list.

| Verb                                 | Response                           | Notes |
|--------------------------------------|------------------------------------|-------|
| `WINLIST`                            | `OK <length>\n<rows>`              | One row per visible top-level window: `<hwnd>\t<x>\t<y>\t<w>\t<h>\t<title>`. |
| `WINFIND <title>`                    | `OK <x> <y> <w> <h>`               | Locate without modifying. |
| `WINACTIVE`                          | `OK <hwnd>\t<x>\t<y>\t<w>\t<h>\t<title>` | The currently-focused top-level window. |
| `WINMOVE <x> <y> <title>`            | `OK <x> <y> <w> <h>`               | Move; does not resize. |
| `WINSIZE <w> <h> <title>`            | `OK <x> <y> <w> <h>`               | Resize; does not move. |
| `WINFOCUS <title>`                   | `OK`                               | Bring to front and activate. |
| `WINCLOSE <title>`                   | `OK`                               | Sends `WM_CLOSE` (or platform equivalent); the window may decline. |
| `WINMIN <title>`                     | `OK`                               | Minimise. |
| `WINMAX <title>`                     | `OK`                               | Maximise. |
| `WINRESTORE <title>`                 | `OK`                               | Un-minimise / un-maximise. |

## UI Automation

A parallel verb namespace that operates on the **accessibility tree** (window hierarchy, control roles and names, invokable patterns) rather than pixels. Where UIA can reach a control, the AI client can find it by semantic name and act on it without simulating clicks — robust to DPI, theming, layout shifts, and language localisation. The pixel-based capture/input verbs remain the fallback for surfaces UIA can't see (canvases, in-browser DOM content, custom-drawn controls, games).

Element identifiers are opaque per-connection integers issued by the agent. `ELEMENTS` rebuilds the connection's id space (older ids are released); `ELEMENT_AT` and `ELEMENT_FIND` add new ids without invalidating existing ones. Ids never persist across reconnects.

Every row uses a single tab-separated layout:
`<id>\t<x>\t<y>\t<w>\t<h>\t<role>\t<name>\t<value>\t<flags>`

- `<role>` — short token from the UIA control type, lowercased: `button`, `edit`, `menu`, `menuitem`, `checkbox`, `combobox`, `tab`, `tree`, `treeitem`, `link`, `text`, `image`, `pane`, `window`, `dialog`, `slider`, `progressbar`, `unknown`, etc.
- `<name>` — the element's accessible name (`UIA_NamePropertyId`). Tabs and newlines in the source are replaced with spaces so the row stays single-line.
- `<value>` — value of the element's `ValuePattern` (edit fields, sliders) when present, else empty.
- `<flags>` — comma-separated subset of `enabled`, `focused`, `offscreen`, `password`, `selected`, `checked`, `expanded`. Empty when none apply.

Availability is advertised via the `ui_automation` capability flag in `INFO` (`uia` on windows-modern, `msaa` on the legacy target when implemented, `no` otherwise).

| Verb                                      | Response                                  | Notes |
|-------------------------------------------|-------------------------------------------|-------|
| `ELEMENTS [<x> <y> <w> <h>]`              | `OK <length>\n<rows>`                     | Enumerate elements. Whole-screen scope by default; restrict with the optional rect (faster, often what the AI wants — "what's on this dialog?"). Filters to interactable / named elements; bare panes and decorative containers are dropped. |
| `ELEMENT_AT <x> <y>`                      | `OK <row>` or `ERR not found`             | Hit-test: returns the leaf element at the given screen coordinate. Useful for "I clicked here, what was it?" debugging and for the model to confirm targets before invoking. |
| `ELEMENT_FIND <role> <name-substring>`    | `OK <row>` or `ERR not found`             | Single-result search by role token + case-insensitive substring match on `<name>`. Convenience for "find the OK button" without enumerating the whole tree. |
| `ELEMENT_INVOKE <id>`                     | `OK` / `ERR not invokable` / `ERR id`     | Calls `IUIAutomationInvokePattern::Invoke` — fires the control's primary action (button click, link follow, menu item) without simulating a click. |
| `ELEMENT_FOCUS <id>`                      | `OK` / `ERR id`                           | Gives keyboard focus to the element. |
| `ELEMENT_TREE <id>`                       | `OK <length>\n<rows>`                     | Recursive descent under `<id>`. Each row carries a leading `<depth>` column: `<depth>\t<id>\t<x>\t<y>\t<w>\t<h>\t<role>\t<name>\t<value>\t<flags>`. The element at `<id>` itself is depth 0; children depth 1, and so on. Useful for "what's inside this menu / list / dialog?" |
| `ELEMENT_TEXT <id>`                       | `OK <length>\n<bytes>` / `ERR id`         | Read text content. Tries `TextPattern` (rich edits, documents) first, falls back to `ValuePattern` (simple edit fields), falls back to the element's accessible name. Empty payload is valid — distinct from `ERR id`. |
| `ELEMENT_SET_TEXT <id> <length>\n<bytes>` | `OK` / `ERR no value pattern` / `ERR id`  | Write into an editable control via `ValuePattern::SetValue`. More reliable than synthesising `KEYS` (handles password fields, IME-aware controls, locked formatting). |
| `ELEMENT_TOGGLE <id>`                     | `OK` / `ERR not toggleable` / `ERR id`    | Checkboxes and toggle buttons via `TogglePattern::Toggle`. |
| `ELEMENT_EXPAND <id>`                     | `OK` / `ERR not expandable` / `ERR id`    | Combo boxes, tree nodes, expandable menu items via `ExpandCollapsePattern::Expand`. |
| `ELEMENT_COLLAPSE <id>`                   | `OK` / `ERR not expandable` / `ERR id`    | The reverse — `ExpandCollapsePattern::Collapse`. |

## System

| Verb                                 | Response                          | Notes |
|--------------------------------------|-----------------------------------|-------|
| `ENV`                                | `OK <length>\n<rows>`             | One row per variable: `<name>=<value>`. |
| `ENV <name>`                         | `OK <value>`                      | Single variable; `ERR not set` if absent. |
| `IDLE`                               | `OK <seconds>`                    | Seconds since last user input. Lets clients avoid stomping on an active human. |
| `DRIVES`                             | `OK <length>\n<rows>`             | One row per filesystem root: `<path>\t<type>` where type ∈ `fixed`, `removable`, `remote`, `cdrom`, `ramdisk`, `unknown`. |
| `LOCK`                               | `OK`                              | Locks the workstation (interactive session only). |

## Power

Gated by capability flag `power=yes`. Absent from `CAPS` when the agent is started without power opt-in. Connection closes after `OK`.

| Verb        | Response             |
|-------------|----------------------|
| `LOGOFF`    | `OK` then close      |
| `REBOOT`    | `OK` then close      |
| `SHUTDOWN`  | `OK` then close      |

## Errors

`ERR <reason>` — short single-line message. Reason text is for humans; the leading `ERR` is the only thing clients should branch on. Where a verb is known but not implementable on this platform, agents return `ERR unsupported` so clients can distinguish "wire-level rejection" from "tried and failed". Long-running verbs interrupted by `ABORT` from another connection return `ERR aborted`.
