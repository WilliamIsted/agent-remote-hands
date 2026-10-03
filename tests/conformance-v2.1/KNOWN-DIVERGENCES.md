# Conformance v2.1: known divergences (windows-classic)

This directory holds the **v2.1 suite** used to test `windows-classic`, which stays on protocol 2.1 by design.

- **Vendored files:** every file except `test_classic_v21.py` and this ledger is a verbatim copy of the `protocol` repo's `tests/conformance/` at tag **`v2.1.0-rc.3`**. Don't hand-patch them. Where a vendored test is wrong, fix it in the protocol repo and re-copy.
- **Additive tests:** `test_classic_v21.py` adds classic-only assertions for the fixes in [`docs/classic-v2.1-conformance.md`](../../docs/classic-v2.1-conformance.md).
- **Not the v2.2 suite:** the `tests/conformance/` suite next to this one is the v2.2 resync. It can't drive classic, because a v2.2 hello gets `protocol_mismatch`.

Run it against the test VM only, never the host PC:

```bash
python -m pytest tests/conformance-v2.1/ --host <vm-ip> --port 8765 --token-path <tok>
```

## Expected failures

### A: stale vendored test (classic follows the spec)

| Test | Why classic is right |
|---|---|
| `test_system.py::test_power_cancel_requires_extra_risky_tier` | `system.power.cancel.json` has `x-crudx: "U"`, so classic registers it at update tier, the same choice modern made (v2.2 ledger, class A). `test_classic_v21.py::test_power_cancel_is_update_tier` asserts the spec tier. |
| `test_directory.py::test_directory_delete_non_empty_requires_recursive` | The test creates its fixture file with `file.write` on a path that doesn't exist yet. `file.write.json` says "The file must already exist (use `file.create`…)", and the same suite's `test_file_write_missing_returns_not_found` asserts `not_found`. Classic follows the spec, so the fixture step fails. The test should use `file.create`. |

The v2.1 suite only tier-gates `file.download`, so the host-only VM's lack of internet (WinINet 12029) doesn't cause a failure here. It only affects the v2.2 suite through the shim.

## Classic-family divergences (not test failures)

These are spec deviations that the v2.1 suite doesn't assert. They're recorded here so they aren't rediscovered.

| Area | Divergence | Reason |
|---|---|---|
| `process.list` on NT 4 without `psapi.dll` | `ERR not_supported` (outside the v2.1 error set) | No process-enumeration API exists on that configuration. |
| `window.find --match regex` | `ERR invalid_args {reason}` | The C89 build has no regex engine. `substring`, `prefix`, `exact` and `glob` are implemented. |
| `watch.*` (subscription mode) | Registration only; no `EVENT` frames are delivered | The single-threaded classic connection model has no event thread. This predates this work. `watch.registry --until-change` (synchronous) is fully implemented. |
| `screen.capture` | Never composites the cursor (spec default `cursor: true`). `--encoding` defaults to raw bytes, not base64. | There's no cursor overlay on the GDI path. Raw bytes are the v2.1 suite's contract (`test_screen.py` checks the PNG/BMP magic in the payload). |
| `clipboard.get` | Raw text payload, not `{content, format}` | The v2.1 suite's `test_clipboard_round_trip_at_update_tier` asserts the raw bytes. |
| `file.read` | Over 16 MB in one call returns `invalid_args`. Page through the file with `--offset` / `--length`. | The response is built in memory, and classic targets low-RAM hosts. |
| `file.write_at` | Offsets are 32-bit | `SetFilePointer` without the high dword, per the classic family note in the spec. |
