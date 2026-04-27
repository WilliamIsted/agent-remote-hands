# tests/conformance

Wire-protocol conformance suite. Runs against a live agent over TCP and grades it on every verb listed in `PROTOCOL.md`. The same suite runs against every target — windows-nt, windows-9x, windows-modern, future targets — so passing it is the gate for "this implementation interoperates with everything else".

## What it tests

Each test file covers one section of the protocol:

- `test_basics.py` — `PING`, `CAPS`, `INFO`, `QUIT`, unknown-verb handling
- `test_files.py` — `READ`/`WRITE` round-trip, `LIST`, `STAT`, `DELETE`, `MKDIR`, `RENAME`
- `test_screen.py` — `SCREEN`, `MPOS`, BMP validity for `SHOT`/`SHOTRECT`
- `test_input.py` — `MOVE` followed by `MPOS` round-trip, `KEY`/`KEYS`/`CLICK`/`WHEEL` smoke tests
- `test_process.py` — `EXEC`/`WAIT` cycle, `RUN` output capture, `PS`, `KILL`, `SLEEP` timing
- `test_clipboard.py` — `CLIPSET`/`CLIPGET` round-trip
- `test_windows.py` — `WINLIST` row format, `WINACTIVE`, `WINFIND`
- `test_system.py` — `ENV`, `IDLE`, `DRIVES`
- `test_resilience.py` — bad input, oversize payloads, repeated faults — agent must survive

## Skip-on-CAPS

Each test calls `self.require_caps('VERB', ...)` first. If the agent's `CAPS` doesn't list a verb, the test is SKIPPED rather than failed. That way:

- A windows-3.1 agent with no `SHOT` is graded only on the verbs it implements.
- Adding a new verb means: adding it to `CAPS`, adding a test to the suite, and rerunning. Verbs not in `CAPS` are off-contract.

If a verb **is** advertised in `CAPS`, the test runs strictly — the agent must implement it correctly.

## Running

```bash
# Point at the target's IP
export REMOTE_HANDS_HOST=192.168.x.x
python tests/conformance/run.py

# Or pass on the command line
python tests/conformance/run.py 192.168.x.x 8765
```

Output is `unittest`'s standard verbose format: `ok` for pass, `FAIL` with assertion detail, `skipped 'reason'` for verbs the agent doesn't claim to support.

Exit code: `0` if every non-skipped test passed, `1` otherwise.

## Requirements

- Python 3.8+ on the host running the suite. No third-party packages.
- Network reachability to the target on the agent's port (default 8765).
- The target's `INFO` should include `os=` and `protocol=1` — anything else is treated as a malformed implementation.

## Extending

Adding a new verb:

1. Add the verb to `PROTOCOL.md`.
2. Implement it on whichever agent first.
3. Add it to `CAPS` on that agent.
4. Add a test method in the appropriate `test_*.py` file. Start with `self.require_caps('NEW_VERB')`.
5. Run the suite — your new test passes on the agent that implements it, skips on every other.

If a verb is destructive (LOGOFF/REBOOT/SHUTDOWN/LOCK), don't exercise it from the conformance suite. Test that it appears in `CAPS` only when `power=yes` and that it errors when `power=no`.

## Workspace cleanup

Tests that write to disk create a per-test workspace under the agent's `TEMP` directory (`MKDIR rh-conf-<random>`), then delete it via `addCleanup`. A botched run might leave `rh-conf-*` directories behind; safe to delete manually.
