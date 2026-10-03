# windows-classic — v2.1 spec conformance fixes

**Base:** `rebuild/v0.3.0` @ `be7b2b2` (pulled 2026-10-03). **Spec:** `protocol` @ `v2.1.0-rc.3`, `spec/verbs/*.json`.
**Scope:** `agents/windows-classic/` only. Modern and legacy are unaffected.
**Goal:** classic meets its own v2.1 spec, so the rebuild merge criterion holds against an accurate suite: "v2.1 suite passes against windows-classic with only ledgered known-divergences".

## Status

Fixes 0–7 are implemented on `claude/festive-bardeen-opss89`. The code compile-checks and links with mingw-w64 (i686). It **has not been built with VS6 or run on the VM**, so the next step is a guest build plus a run of the suite.

| Piece | Where |
|---|---|
| Accurate v2.1 suite | `tests/conformance-v2.1/`: a verbatim copy of the protocol repo's `v2.1.0-rc.3` suite |
| Classic-only assertions | `tests/conformance-v2.1/test_classic_v21.py` |
| Ledger | `tests/conformance-v2.1/KNOWN-DIVERGENCES.md` |

**Correction to the handover's group 1:** the v2.1 suite itself sends **hyphenated** flags (`--timeout-ms`, `--visible-only false`, `--include-counters`, `--delay-seconds`). The snake_case spellings (`--timeout_ms`) came from the v2.2 → v2.1 shim. Fix 0 therefore makes the parser treat `-` and `_` as the same character instead of renaming flags. Both spellings work.

**Spec vs suite conflicts, and how they were resolved:**

- `power.cancel`: classic follows the spec (update tier), and the suite's test is ledgered as stale.
- `screen.capture`: an unknown format keeps `not_supported` (both the v2.1 and v2.2 suites assert it), and `webp` returns `unsupported_format` (the v2.2 suite).
- The `directory.delete` fixture uses `file.write` to create a file. The test is ledgered as stale.

**Also fixed beyond the handover's list:** spec shapes for `file.exists`, `file.rename` / `directory.rename` (`{renamed, fallback_used}`), `directory.delete` (the v2.1 name, with `directory.remove` kept as an alias), `window.focus` / `window.move`, `process.shell` (`pid` may be null), and `power.cancel` (`cancelled_until_ms` is now the remaining time, not the deadline).

---

## Root causes (from reading the source)

1. **Flag vocabulary predates the spec.** `rh_args_parse` (`verbs/common.c:97`) matches flags exactly against per-verb `RhFlagDef` tables. Classic's tables use the old hyphenated or legacy names (`--timeout-ms`, `--no-atomic`, `--filter`, `--all`, `--include-monitor`, `--no-cursor`). The spec uses `snake_case` (`--timeout_ms`, `--atomic false`, `--pattern`, `--visible_only`, …). That one cause produces all of group 1 and the knock-on `file.wait` / `watch.registry` failures in group 3.
2. **Output emitters predate the spec.** They hard-code `"dir"`, `mtime_unix`, `{windows:[{hwnd}]}`, and the old `system.info` keys.
3. **Error mapping.** Ad-hoc codes (`target_gone`, `not_supported`) and generic `rh_win32_code()` mapping where the spec names specific codes.
4. **Connection verbs aren't advertised.** `system.capabilities` is built from the dispatch table (`connection.c:176-272`). The `connection.*` verbs are dispatched by `strcmp` *before* that table (`connection.c:420-445`), so they work but are never advertised.

---

## Fix 0 — Arg-parser groundwork (do this first)

- **Spec-named flags.** Every `RhFlagDef` uses the spec's property name with `--` prepended. Matching treats `-` and `_` as equal, so `--timeout-ms` (v2.1 suite) and `--timeout_ms` (spec-derived callers) both work.
- **Booleans take a value.** The spec has `atomic` (default `true`) and `visible_only` (default `true`), so `--no-x` cannot express them. Accept both `--flag` (meaning true) and `--flag true|false`. Add `rh_arg_bool(const RhArgs*, int idx, int dflt)`. If a `true`/`false` token follows a boolean flag, consume it. Otherwise treat it as positional.
- **Legacy names.** The classic-only spellings `--filter`, `--all` and `--no-atomic` were dropped. The v2.0 positional forms (e.g. `process.wait <pid> <timeout>`) and `--delay` / `--force` on power verbs are still accepted.
- **Required args.** Where the spec's `required` lists a named arg (e.g. `file.create.path`), accept `--path X` as well as the existing positional form. Prefer one lookup helper, `rh_arg_named_or_pos(a, flag_idx, pos_idx)`, over per-verb special cases.

## Fix 1 — Unrecognised arguments (group 1)

| Verb | Add / rename flags (spec v2.1) | Current classic | Behaviour to implement |
|---|---|---|---|
| `clipboard.set` | `--format` (enum `text`, default `text`) | none | Reject anything other than `text` with `invalid_args`. Response `{format:"text"}`. |
| `directory.list` | `--pattern`, `--limit` | `--recursive` only | Filter entries by glob (`PathMatchSpecA`, or a hand-rolled `*`/`?` matcher if NT 4 shlwapi is a concern). Stop the walk once `limit` entries are emitted. |
| `file.create` | `--path`, `--encoding`, `--atomic` | positional only | Encode `content` per `encoding`. `atomic` writes to a temp file, then `MoveFileA` (fails if the target exists, giving `already_exists`). |
| `file.read` | `--encoding`, `--offset`, `--length` | `--truncate`? (verify) | Shared encoder module (below). |
| `file.write` | `--encoding`, `--atomic` | `--encoding`, `--no-atomic` | Rename `--no-atomic` to `--atomic <bool>`. |
| `file.write_at` | `--encoding`, `--truncate` | `--truncate` | Add `--encoding`. |
| `process.list` | `--pattern`, `--include_counters` | `--filter` | Rename `--filter` to `--pattern`. Counters: `GetProcessTimes` / `GetProcessMemoryInfo` (psapi) / `GetProcessHandleCount` (XP+, else 0, per the family note). |
| `process.wait` | `--timeout_ms` | positional `<pid> <timeout-ms>`, timeout required | **Confirmed in v2.1** (the handover's "unconfirmed" one). Make the timeout optional (absent means `INFINITE`), read it from `--timeout_ms`, and keep `pid` positional. |
| `system.power.shutdown` | `--delay_seconds`, `--force_close_apps`, `--reason` | none | `delay_seconds > 0`: use `InitiateSystemShutdownA` (NT) and return `{scheduled_at}`. 0: keep the existing `ExitWindowsEx` path. |
| `watch.file` | `--recursive` | positional glob | Pass to `ReadDirectoryChangesW(bWatchSubtree)`. On the NT 4 / 9x fallback, use `FindFirstChangeNotification(bWatchSubtree)`. |
| `watch.region` | `--encoding` (`binary`/`base64`, default `binary`), `--interval_ms`, `--until_change` | none | Base64 the frame payload when requested. |
| `watch.registry` | `--watch_subtree`, `--until_change`, `--timeout_ms` (default 30000) | none | `until_change`: single arm plus a wait. Return `{path}`, or `timeout`. Otherwise use subscription mode and return `{subscription_id}`. |
| `window.list` | `--visible_only` (default **true**), `--pid`, `--pattern`, `--include_monitor` | `--filter`, `--all` | `--all` becomes `--visible_only false`. `--filter` becomes `--pattern`. Add the `--pid` filter. Add `monitor_index` via `MonitorFromWindow` (resolved dynamically, so NT 4 always reports 0). |
| `file.wait` | `--timeout_ms` (default 30000) | `--timeout-ms` | Rename. |

**Shared work:** create `verbs/encoding.c` with `utf-8`, `utf-16le`, `utf-16be`, `ascii`, `latin-1`, `cp1252` and `binary` (base64). It is used by `file.read`, `file.write`, `file.write_at` and `file.create`. Echo `encoding` in every write response. An unknown encoding returns `invalid_args`, never a silent fallback.

## Fix 2 — Response shapes (group 2)

| Verb | Spec v2.1 shape | Classic today | Change |
|---|---|---|---|
| `system.info` | required: `family, agent, agent_protocol, os_name, os_version, cpu_arch, integrity, uiaccess, hostname, screens, capabilities, current_tier` | `name, version, protocol, os, arch, …, monitors` (`system.c:63-116`) | Rewrite the emitter. `family:"windows-classic"`. `os_name`/`os_version` from `GetVersionEx`. `cpu_arch` from `GetNativeSystemInfo` (resolved dynamically, falling back to `GetSystemInfo`). `integrity`: `"medium"` on 5.1+, `"none"` before. `screens` replaces `monitors`. Keep any extra keys only if the schema allows them (check `additionalProperties`). |
| `file.stat` | `type` ∈ `file/directory/link/other`, `size`, `mtime_unix_s`, `atime_unix_s`, `ctime_unix_s`, `flags` | `"dir"`, `mtime_unix` (`file.c:262-264`) | Rename the keys and add atime/ctime/flags. Detect `link` via `FILE_ATTRIBUTE_REPARSE_POINT` on NT 5+. |
| `file.exists` / `directory.list` entries | same `type` enum | `"dir"` (`file.c:305`, `directory.c:104-107`) | `"directory"`, `mtime_unix_s`. |
| `directory.stat` | `type:"directory"` (only), `entry_count`, `mtime_unix_s`, … | `"dir"`, `mtime_unix` (`directory.c:186-188`) | Rename. A non-directory returns `not_a_directory`. |
| `window.list` | **bare array** of `{handle, title, pid, bounds, monitor_index?}` | `{windows:[{hwnd,…}]}` (`window.c:75,127`) | Unwrap the array and rename `hwnd` to `handle` (`win:0x…`). `bounds` is a `Bounds` object. Also fixes the `KeyError: 0` failures. |
| `file.write` / `file.write_at` / `file.create` | `{bytes_written, encoding}` (+ `new_size` for `write_at`) | empty body | Emit JSON. |
| `directory.create` | `{created:true}` | empty body | Emit JSON. |
| `clipboard.set` | `{format}` | (verify) | Emit JSON. |
| `system.power.shutdown` | `{scheduled_at}` iff `delay_seconds > 0`, else `{}` | (verify) | Emit JSON. |

**Optional, not a bug:** v2.1's `capabilities` is open-ended, so `wake_timer_supported` and `input_settings` aren't required. However, the v2.1 classic family block lists both under `typical_capabilities`, so they're cheap to add while rewriting `system.info`. They're also needed for 2.2 later.

## Fix 3 — Error codes (group 3)

| Verb | Spec errors | Classic today | Change |
|---|---|---|---|
| `screen.capture` | `not_found, unsupported_format, permission_denied` | `not_supported` (`screen.c:431`); `target_gone` for a bad `--window` (`screen.c:468`) | Use `unsupported_format`, and `not_found` for a dead window. |
| `window.focus` / `window.close` (and `window.move`, `window.state` for consistency) | `not_found, invalid_args` (+ `uipi_blocked` on close) | `target_gone` (`window.c:229,273,300,329`) | Use `not_found` for a dead or unknown handle and `invalid_args` for an unparsable one. |
| `process.shell` | `not_found, permission_denied, user_cancelled, no_handler, invalid_args` | generic `rh_win32_code(GetLastError())` (`process.c:~340`) | Map explicitly: `ERROR_NO_ASSOCIATION` / `SE_ERR_NOASSOC` gives `no_handler`. `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` give `not_found`. `ERROR_CANCELLED` gives `user_cancelled`. Only real `ERROR_ACCESS_DENIED` gives `permission_denied`. Verify on the VM what an unknown `--verb` actually returns, since it was surfacing as `permission_denied`. |
| `process.list` (pre-NT-5 path) | `permission_denied, invalid_args` | `not_supported` (`process.c:222`) | `not_supported` isn't in the v2.1 set. Ledger it as a known divergence (NT 4 without psapi), or route it to `permission_denied`. **Recommendation: ledger it.** |
| `file.wait`, `watch.registry` → `timeout` | — | `invalid_args` | Fixed by Fix 0 and Fix 1 (flag rename). No separate work. |
| `file.download` 404 → 12029 | — | — | **Environment** (host-only VM). Ledger it, don't fix. |

## Fix 4 — Advertise the `connection.*` verbs

Add `connection.hello`, `connection.tier_raise`, `connection.tier_drop`, `connection.reset` and `connection.close` to the capability listing. Two options:

- **(a)** Put them in the dispatch table, with the pre-hello gate still handled ahead of it.
- **(b)** Have `rh_verb_system_capabilities` prepend a static `connection.*` list.

**Recommendation: (a).** It keeps the "table is the single source" invariant from the comment at `system.c:149`. Also check the response shapes: `tier_drop` returns `{new_tier}`, and `reset` / `close` return `OK 0`.

## Fix 5 — Power tier mismatches (Task 0)

| Line | Verb | Today | Spec `x-crudx` | Change |
|---|---|---|---|---|
| `connection.c:182` | `system.power.lock` | `RH_TIER_READ` | X | `RH_TIER_EXTRA_RISKY`. Already fixed in your local tree; not on the remote branch yet. |
| `connection.c:188` | `system.power.cancel` | `RH_TIER_EXTRA_RISKY` | U | `RH_TIER_UPDATE`. Matches what modern did in Task 0 (ledger class A). |

**Test:** `test_power_cancel_requires_extra_risky_tier` is stale (ledger class A), so expect it to keep failing until the Protocol-repo PR lands. Add a classic test that asserts `power.cancel` is allowed at update tier and `power.lock` is denied below extra_risky.

## Fix 6 — `file.write` / `file.write_at` must not create files

v2.1.0-rc.3 and v2.2 have the same wording. For `file.write`: "The file must already exist (use file.create…)", with `not_found` as an error. For `file.write_at`: "returns ERR not_found if absent".

| Line | Today | Change |
|---|---|---|
| `file.c:152` (`file.write`) | `CREATE_ALWAYS` | `TRUNCATE_EXISTING`. For the atomic path, probe for the target's existence first, then write the temp file and replace it. |
| `file.c:204` (`file.write_at`) | `OPEN_ALWAYS`, or `CREATE_ALWAYS` when `--truncate` is given at offset 0 | `OPEN_EXISTING`. Apply truncate with `SetEndOfFile` after opening. |

- **Error mapping:** map `ERROR_FILE_NOT_FOUND` and `ERROR_PATH_NOT_FOUND` to `not_found`.
- **Compatibility:** both flags exist back to NT 4 and Win95.
- **Leave `file.download` alone:** its `CREATE_ALWAYS` (`file.c:820`) is correct.

**Tests:** run `test_file.py::test_file_write_missing_returns_not_found`. Add a `write_at` equivalent. The old v2.1 suite never covered this, which is why it passed 74/0.

## Fix 7 — Oversized header: `wire_desync` instead of dropping the connection

When a header line is over 65,535 bytes, `protocol.c:181` returns `RH_PROTO_ERR`, and the connection loop closes the socket.

v2.1's `framing/10-behaviour-notes.md:29` says the agent SHOULD return `ERR wire_desync`, discard the inbound buffer, and let the client recover with `connection.reset`. So today's behaviour isn't a spec violation, but it isn't what the spec asks for.

Changes (about 20 lines):
- **`protocol.h`:** add `RH_PROTO_DESYNC (-3)`.
- **`protocol.c`:** when the header overflows, enter skip mode. Discard input up to and including the next `\n`, then return `RH_PROTO_DESYNC`. Keep the 65,535-byte cap on the buffer so skip mode can't grow it.
- **`connection.c` loop:** on `RH_PROTO_DESYNC`, send `ERR wire_desync` and continue the loop instead of closing.
- **Payload-length overflow:** the `protocol.c:189` / `:204` paths stay fatal. Without a trustworthy length there's no safe resync point.

Because the reader resyncs at the next newline, the next request just works, and clients don't strictly need `connection.reset`. Fix 4 still adds it so the spec's recovery path exists.

**Test:** send a 70 KB header line. Expect `ERR wire_desync`, then a `system.health` on the same socket returning `OK`.

**Not fixable in the agent:** the v2.2 registry test puts a 2.8 MB base64 value in the arguments. Over v2.1 it still fails, because large data belongs in the payload. That's a shim limitation, so ledger it.

---

## Out of scope / not applicable

- **Group 4:** framing (ws), MCP `tools/list` / `serverInfo`, and `framing_unsupported`. Classic stays on 2.1 by design. The 3 hello failures are a shim artefact.
- **Group 5:** the ~5 shim timeouts. They need a better v2.1 payload shim before they can be triaged, so re-run them after Fixes 0–4. Most may disappear once flags parse.

## Test plan

1. **Use the accurate v2.1 suite.** It's the protocol repo's own `v2.1.0-rc.3` suite, vendored as `tests/conformance-v2.1/`. No shim is needed.
2. **Add a classic-specific test per fix** where the shared suite doesn't already cover it, especially: the boolean `--flag false` form, `window.list` bare array, `system.info` required keys, and `connection.*` present in capabilities.
3. Run against the VM only (never the host PC): `python -m pytest tests/conformance-v2.1/ --host <vm-ip> --port 8765 --token-path <tok>`.
4. **Exit criterion:** the ~60–65 classic failures go to 0. The only remaining failures are ledgered (group 4 n/a, `file.download` env, the NT 4 `process.list` divergence).

## Suggested commit order

1. Fix 0 (parser + bool helper). Mechanical, and unblocks the rest.
2. Fix 1 + the encoding module.
3. Fix 2 (shapes), then Fix 3 (errors).
0. Fixes 5, 6 and 7 first. They're small, independent, and already diagnosed.
4. Fix 4 (connection verbs).
5. Suite + ledger update.

Per CLAUDE.md, each wire-visible change touches `PROTOCOL.md` (where it still describes the old classic shape), the agent source, and a conformance test.

## Open decisions for you

1. ~~Drop the hyphenated flag aliases?~~ This question no longer applies: hyphenated names are the v2.1 suite's own spelling, so both spellings are accepted.
2. NT 4 `process.list` `not_supported`: it's recorded as a known divergence for now. Remap it if you'd rather.
3. Does this block the rebuild merge? Given the stated merge criterion, it would, unless the criterion is reworded to "against main's suite".
