# LLM Operator Cheatsheet

Recipes for LLMs (or scripts) driving this agent over the v2.2 wire protocol. Each
entry is *Problem → Recipe*. Read top-to-bottom once; reach for specific sections
as situations arise.

The wire spec lives in `PROTOCOL.md` and the pinned `protocol/` submodule; this
file is operator-side guidance — what *works in practice* when you're driving a
real Windows host through the verbs.

---

## 1. Waiting for things to happen

**Don't poll from the client.** The agent has native wait verbs that block
server-side on kernel/UIA notifications and return in one round-trip:

| Goal | Verb |
|---|---|
| Wait for a file to exist (download, install artifact) | `file.wait` (`glob`, `timeout_ms`) |
| Wait for a process to exit (installer finished, app closed) | `process.wait` (`pid`, `timeout_ms`) |
| Wait for a UIA element to appear (or transition state) | `element.wait` (`name`/`role`/`automation_id`, `flags_required`, `timeout_ms`) |

**`element.wait flags_required=["enabled"]`** is the right tool for "wait until
this button is *clickable*" (e.g., wait for a wizard's terminal button to enable
after the install copy phase). The `glob` for `file.wait` supports `*` within a
single path component — `C:\Program Files*\Vendor\App.exe` matches both
`Program Files` and `Program Files (x86)`.

Your TCP socket timeout must exceed `timeout_ms`. If your client defaults to
30 s, a 4-minute `file.wait` will appear to fail when the agent is still
correctly blocked — bump the client's read timeout.

---

## 2. Driving installer wizards

Most Windows installers (NSIS, MSI, Inno, Qt-based) expose a UIA tree of the
dialog's controls. Drive them by **name + role**, not by pixel coordinate, when
you can.

```text
process.start argv=[<Installer.exe>]                  # elevated agent → no UAC; medium-IL agent → 740
element.wait   name="Welcome" / role="window" …       # ensure wizard up
element.find_invoke name="Next" role="button"         # advance
element.find_invoke name="I Accept" role="button"     # EULA pages
element.find_invoke name="Install" role="button"      # start copy phase
element.wait   name="Finish" role="button" flags_required=["enabled"]
element.find_invoke name="Finish" role="button"       # **always click the terminal button**
```

Key gotchas:

- **The terminal button is not always called `Finish`.** Cross-vendor it varies:
  `Finish`, `Next`, `Next >`, `Done`, `Close`, `Complete`, `OK`, localized.
  Sequentially `element.wait` each candidate, or — until disjunctive matching
  lands as a verb-level feature — periodically `element.list` the dialog region
  and look for any enabled button whose name is in your candidate set.
- **License pages disable Next until you accept.** If `element.find_invoke
  "Next"` silently no-ops, run `element.list region=<dialog> full=true` and read
  the Next button's `flags`. Empty `flags` (no `enabled`) ≠ broken UIA — it's
  a disabled control. Click the "I accept" radio or check the EULA box first.
- **`element.find`/`element.wait` `name` is a case-insensitive substring.**
  Pick the most distinctive phrase available so you don't hit chrome / taskbar
  buttons that share words with the wizard.
- **Installers often launch the app after the terminal click.** If you don't
  want that, look for and uncheck a `Run <app>` checkbox before clicking the
  terminal button.
- **Verify install by file artifact, not by wizard close.** `file.exists` on
  the installed binary is the definitive completion signal — but only AFTER
  you've clicked the terminal button, because many installers finalize side
  effects on that click. The binary often lands on disk *before* the wizard
  reaches its final page.

---

## 3. Element discovery patterns

```text
element.find                  →  first match in tree order, by name/role
element.find_invoke           →  find + UIA Invoke pattern in one verb
element.list region=<rect>    →  enumerate everything inside a screen rect
element.list full=true        →  include flags + value (enabled/checked/etc.)
element.at x=<n> y=<n>        →  UIA element at a coordinate (hit-test)
element.wait                  →  block until match (with optional flags_required)
element.tree                  →  dump UIA subtree (heavy; debug only)
```

**Scoping a search:**

- `element.find root=<window-handle>` — *intended* way to scope. Works well on
  native Win32 dialogs. **Currently flaky on Chromium HWNDs and Qt installer
  windows** (returns `target_gone` for handles `window.list` reports as
  valid). Until that's fixed, prefer `element.list region=<dialog-bounds>`
  and filter client-side.
- For web content, the first `element.find` after page load can be slow
  (Chrome builds its a11y tree lazily). Use a long `timeout_ms` (≥ 15 s)
  and ensure your TCP read-timeout is larger still.

**Avoiding false matches with substring `name`:**

- Taskbar buttons' accessible names ARE the full window title — so a generic
  word like "Download" or "Install" will match the taskbar entry first.
  Choose a longer substring that's unique to the target.
- `element.list region=<page-content>` excludes the taskbar (y >= ~screen
  height − 50) and the address-bar zone (y < ~140 in Chrome) — use it.

---

## 4. Process execution and elevation

| Verb | What it does | UAC behaviour |
|---|---|---|
| `process.start` (argv array) | `CreateProcess` | **No** UAC. Fails `ERROR_ELEVATION_REQUIRED` (Win32 740) on `requireAdministrator` manifests. |
| `process.shell` (path, args, verb) | `ShellExecuteEx` | `verb=runas` → triggers UAC prompt if needed. |
| `process.kill` | `TerminateProcess` | Delete-tier verb. |
| `process.wait` | `WaitForSingleObject` | Read-tier; returns `exit_code`. |

**Elevation strategy:**

1. **Most reliable: launch the agent itself elevated.** Once the agent runs at
   High IL, any installer it launches via `process.start` inherits the elevated
   token — *zero subsequent UAC prompts*. Trigger this once via
   `process.shell verb=runas` on the agent binary (a single UAC click).
2. **Medium-IL agent:** `process.start` will fail 740 on real installers.
   `process.shell verb=runas` will work but raises a UAC prompt on the secure
   desktop, which **`SendInput` cannot reach regardless of agent IL** (this
   is by Windows design — the secure desktop is intentionally immune to
   synthetic input). The prompt must be confirmed by a human at the console,
   or VM UAC must be pre-configured to not prompt.

**The agent CANNOT click UAC prompts.** Not a bug — secure-desktop isolation.
Either pre-elevate the agent or accept that elevation requires a human.

---

## 5. Web content (browsers)

Chromium-based browsers are the most awkward UIA surface:

- The HWND → UIA-root resolution fails on the top-level Chrome window
  (`root=win:…` → `target_gone`). Use `element.list region=` scoped to the
  page area instead.
- Chrome only fully populates the accessibility tree once a UIA client
  queries it; the *first* query is slow.
- Page content below the visible fold may not be in the a11y tree until
  scrolled into view. `input.mouse.scroll` to surface below-fold content
  before searching.
- `process.start chrome.exe <URL>` while Chrome is running opens a **new
  tab**, not a new window. Window handles persist; tab focus may not. Be
  ready to identify which tab the agent is operating on via window/tab title.
- For known direct file URLs (an installer's CDN link), navigating Chrome
  directly to the `.exe` URL is still browser navigation — the browser
  fetches and saves to Downloads, exactly as a click on a `<a href="…exe">`
  would. This is a clean way to bypass JS-heavy landing pages that hide the
  download CTA behind nav.

---

## 6. Window management

| Verb | Notes |
|---|---|
| `window.list` | All top-level windows with `handle`, `title`, `bounds`, `pid`. Handles are **not stable** across navigation / tab changes — always re-query before reuse. |
| `window.find` | Filtered list. |
| `window.focus` | Bring to foreground. May return `focused_status:"lock_held"` when Windows refuses foreground (e.g., during another app's foreground-lock window). The raise *usually* still happens; flag is informational. |
| `window.move` | Reposition / resize. Moving to negative coordinates (e.g., `x=-2000,y=-2000`) **effectively hides a window without closing it** — useful for shoving login/launcher pop-ups out of the way without dismissing them. |
| `window.close` | Sends `WM_CLOSE`. Some apps (Qt installer wizards) ignore it — click their close button or terminal action instead. |
| `window.state` | **Read-only** in v2.2 — returns the current state. There is currently *no* setter verb for minimize/maximize/restore. Workaround: `window.focus` + `window.move` cover most needs. |

---

## 7. Verifying state textually (avoid screenshots for control flow)

`screen.capture` is essential for the user to *see* what's happening, but
*the LLM operator should not need it for decisions*. Use textual probes:

| Question | Verb |
|---|---|
| Did the installer finish? | `file.exists` / `directory.exists` on the installed binary |
| Is a wizard at its terminal page? | `element.find` on the candidate-set terminal button names with `flags_required=["enabled"]` |
| Is the app running? | `process.list` filtered by name, or `process.wait` on a known pid |
| What window is foreground? | (no direct verb yet — known gap) inspect `window.list`; the highest-z-order window is *usually* but not always foreground |
| What buttons are on this dialog? | `element.list region=<window-bounds> full=true` (includes flags) |

Screenshots are appropriate when text genuinely can't disambiguate (e.g., a
page's CTA button name is unknown). Prefer **region** captures over full-screen
to keep them cheap. The base64 image content item is what MCP-stdio carries;
`screen.capture --encoding binary` returns the Shape B side-channel raw bytes
for clients that consume it.

---

## 8. Tier model (CRUDX)

The agent enforces a five-level tier per connection: `read < create < update <
delete < extra_risky`. Raise once with `connection.tier_raise <tier> <token>`
and higher tiers subsume lower. Mapping common driving needs:

| Verb category | Tier |
|---|---|
| `system.info`, `window.list`, `element.find`, `screen.capture`, `file.exists` | Read |
| `file.create`, `file.download`, `process.start` | Create |
| `input.mouse.*`, `input.keyboard.*`, `element.find_invoke`, `window.focus`, `window.move`, `file.write` | Update |
| `file.delete`, `directory.delete`, `process.kill` | Delete |
| `system.power.*` (reboot, shutdown, lock, hibernate, sleep) | Extra Risky |

Driving installers typically requires Update tier (which subsumes Create and
Read). Raise once at hello+1 and you're set for the whole session.

---

## 9. Common pitfalls (one-line callouts)

- **`process.start` returns only `{pid}`** — no stdout capture. For diagnostic
  output, redirect to a file: `argv=["cmd.exe","/c","ipconfig > C:\\arh\\net.txt"]`
  then `file.read` (note: `file.read` content delivery may be deferred — check
  `KNOWN-DIVERGENCES.md`).
- **`process.shell verb=runas` opens windows minimized** — the agent's
  `nShow` defaults aren't `SW_SHOWNORMAL`. Caller may need to `window.focus`
  the child after launch (or accept invisible operation).
- **Handles change.** Chrome's HWND can change across tab navigation; an
  installer wizard creates new handles per page. `window.list` before each
  scoped operation; never cache a handle for long.
- **Substring name collisions.** "Install" matches taskbar buttons named
  "… Setup - 1 running window". Scope by `region` or pick a longer phrase.
- **Disabled buttons accept `Invoke`** — at least in many UIA implementations.
  Always check `flags` (`element.list full=true`) before assuming a click did
  anything visible.
- **Synthesized clicks at known UIA bounds can land elsewhere** if the window
  is minimized or another window came forward. `target_handle` in the
  `input.mouse.click` response tells you which window the click actually hit.
- **`screen.capture` may return `not_supported / capture produced empty frame`**
  when the secure desktop is active (UAC prompt on screen) — that's a signal
  the secure desktop is up and the agent is "blind" to it.

---

## 10. Known divergences

The pinned conformance suite has known disagreements with the current agent
implementation (some are stale tests, some are agent gaps, some are
deferred). See `tests/conformance/KNOWN-DIVERGENCES.md` for the classified
list. Notable for operators:

- **`watch.*` over MCP-stdio** — subscription EVENT-frame streaming is flaky
  in v2.2; prefer the one-shot `*.wait` verbs.
- **`file.read` / `file.write` / `file.write_at` content delivery** — phase-2b
  deferred. The verbs return `not_found` correctly for missing paths but
  `invalid_args` with `reason: file_content_phase2b` for present paths.

---

*Found a pattern that needs to be in this file? Open a PR. Found a real
agent defect you worked around? File it under the `agent-feedback` label.*
