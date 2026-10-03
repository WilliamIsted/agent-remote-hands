# Conformance suite

The conformance suite is the executable contract for the v2 wire protocol. Any agent claiming to speak protocol version 2 must pass it.

## Running

Against a local agent on the default port:

```bash
pip install pytest
python tests/conformance/run.py 127.0.0.1
```

Against a remote agent:

```bash
python tests/conformance/run.py 192.168.1.42 8765
```

Or invoke `pytest` directly for fine-grained control:

```bash
pytest tests/conformance --host 192.168.1.42 --port 8765 -v
```

## What's tested

One file per namespace under `tests/conformance/`. Each test file:

- Skips its tests if the agent does not advertise the relevant verbs (per `system.capabilities`).
- Uses the `client` fixture (read tier — the default on a fresh hello), or one of the elevated-tier fixtures `create_client` / `update_client` / `delete_client` / `extra_risky_client` (all need `--token-path`).

## Token elevation

Tier-raising tests read the agent's elevation token from `%ProgramData%\AgentRemoteHands\token` by default. Override with `--token-path` or the `REMOTE_HANDS_TOKEN_PATH` environment variable. Tests that need elevated tiers are skipped if the token file is unreadable.

## What's not tested

- **Actual synthetic input** (`input.click`, `input.type`, etc.). These are tier-gated tested but exercising real input would visibly perturb the host running the suite. Manual / interactive testing covers that.
- **Actual reboot / shutdown / logoff.** Tier-gated tested.
- **EVENT-frame delivery for `watch.*`.** The subscription-registration path is tested; verifying real event delivery requires deliberate triggers (process spawn-and-die, etc.) which the harness does not currently orchestrate.

## Layout

```
tests/conformance/
├── README.md             (this file)
├── run.py                (entry point: `python run.py <host> [port]`)
├── conftest.py           (pytest fixtures)
├── wire.py               (Python wire-protocol client)
├── test_connection.py
├── test_system.py        (system.* + system.power.*)
├── test_window.py
├── test_input.py         (top-level input.*)
├── test_input_mouse.py   (input.mouse.*  — split out v2.1.0-rc.3)
├── test_input_keyboard.py (input.keyboard.* — split out v2.1.0-rc.3)
├── test_element.py
├── test_vision.py        (vision.ocr — added during the v2.1.0 rc cycle)
├── test_clipboard.py
├── test_directory.py
├── test_file.py
├── test_process.py
├── test_registry.py      (registry.value.* + registry.key.*)
├── test_screen.py
└── test_watch.py
```

## Adding a verb

When wire-protocol changes land:

1. Update `spec/verbs/<verb>.json` (per-verb spec — feeds `dist/PROTOCOL.md` §4) or `spec/framing/*.md` (framing-section content). Run `python Tools/gen.py` to regenerate `dist/`.
2. Implement the verb in `agents/<target>/...`.
3. Add a test case in the appropriate `test_<namespace>.py` (or a new file for a new namespace).
4. The test MUST use `needs_verb(capabilities, "<verb>")` so older agents that don't implement it are skipped, not failed. `tests/check_spec.py` enforces this — every verb in `spec/verbs/` must have at least one `needs_verb(capabilities, "<verb>")` call somewhere under `tests/conformance/test_*.py`, or the spec validator will fail.
