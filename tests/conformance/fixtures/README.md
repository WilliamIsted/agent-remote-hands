# Conformance fixtures

Reference implementations of operations the agent performs, written against
the same Win32 surface the agent uses. The conformance suite can call these
to cross-check that an agent's behaviour matches a known-good baseline.

The fixtures are *not* tests of the fixtures themselves — they are PS
implementations of the canonical Win32 idiom for a given operation, so the
test suite has something stable to compare against.

## What's here

| File | Purpose |
|---|---|
| `ps_input_click.ps1` | Synthetic click using the canonical coupled-`SendInput` pattern. The form `input.click` uses internally. Useful as the "right" baseline when investigating click-position discrepancies. |
| `ps_screenshot_with_cursor.ps1` | Screenshot that composites the OS mouse cursor onto the bitmap. The form `screen.capture` produces by default. Useful as a pixel-level reference. |

Both scripts work standalone — invoke them from any PowerShell session. They
don't require the agent to be running. They're documentation as much as
fixtures.

## Why this lives in the repo

The bug history that produced `input.click` and `screen.capture --include-cursor`
both came from comparing the agent's output to a hand-rolled PowerShell
reference and noticing differences. Keeping the references checked in lets
future contributors regenerate the comparison without rediscovering the
canonical Win32 form from scratch — which, per the rc.8 input-click drift
issue, is harder than it looks.
