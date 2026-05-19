# Conformance — Known Divergences

Operational note for anyone running the conformance suite against a v0.3.0
agent. A clean VM-hosted run currently reports **26 failed / 167 passed /
9 skipped** plus **`test_rebuild_v030.py` 25/25 passed**. **None of the 26
failures is a v0.3.0 (R1–R4) regression** — each is classified below.

Run it the sanctioned way (never the host PC):

```
python -m pytest tests/conformance/ --host <vm-ip> --port 8765 --token-path <tok>
```

The suite under `tests/conformance/` (except `test_rebuild_v030.py` and
`fixtures/`) is a **verbatim resync of the pinned `protocol/` submodule
suite** (sha256-identical). It is intentionally NOT hand-patched: where a
submodule test is wrong, the agent is spec-correct and the *test* needs a
Protocol-repo PR — out of scope for the v0.3.0 agent rebuild. Re-resync
from the submodule whenever the submodule pin is bumped.

## Classification

### A — Agent spec-correct; submodule test stale (needs Protocol-repo PR)

| Test(s) | Why the agent is right |
|---|---|
| `test_clipboard.py::test_clipboard_round_trip_at_update_tier` | `clipboard.get.json` x-output is `{content, format}` (both required, `additionalProperties:false`). The agent returns exactly that. The test asserts the bare set-bytes instead. |
| `test_screen.py::test_screen_capture_full / _png_signature / _bmp_signature / _region / _invalid_format / _no_cursor_flag_accepted / _webp_n_shorthand_rejected` | Two compounding stale-suite issues: (1) framing §1.6.6 mandates `screen.capture` return an **MCP image content item** `{"type":"image","data":"<b64>",...}`; the submodule `wire.py.request()` only extracts `type:"text"` items, so it *structurally* cannot read a spec-correct response (R4a made the agent spec-correct — verified by `test_rebuild_v030.py`). (2) The tests send `--region 0,0,100,100` → `{"region":"0,0,100,100"}` (string), but `screen.capture.json` `region` is `type:object {x,y,w,h}` `additionalProperties:false`; the agent correctly rejects the string. |
| `test_vision.py::test_vision_ocr_region_returns_required_shape` | Same `region` string-vs-object stale-test issue as screen. |
| `test_system.py::test_power_cancel_requires_extra_risky_tier` | Task 0 intentionally set `system.power.cancel` → `Tier::Update` per spec `x-crudx:"U"`. Agent is spec-correct; the test asserts the old `extra_risky`. |

### B — Out-of-scope architecture (not fixable in the v0.3.0 agent rebuild)

| Test(s) | Why |
|---|---|
| `test_system.py::test_verbs_returns_verbs_object / _entries_are_strict_tool_defs / _superset_of_capabilities` | `system.verbs` is implemented on the wire but not exposed on the MCP `tools/*` surface. That surface is the `mcp-server/` bridge, explicitly out of scope for this run (and not yet built). |

### C — Genuinely-deferred behaviour (Phase-2b content delivery)

| Test(s) | Why |
|---|---|
| `test_file.py::test_file_create_then_write_then_read` | Composite; `file.create` works (R2), but the `file.write` step exercises content delivery which is genuinely Phase-2b-deferred (`invalid_args {reason:file_content_phase2b}`). R4a fixed the *missing-path* case (`not_found`); content delivery remains deferred by design. |

### D — Pre-existing, in the R1 baseline; NOT a v0.3.0 regression; root-cause triage deferred

These failed on the clean R1-committed baseline (verified by a stash→rebuild→run
comparison) and are unchanged by R1–R4. Per-test agent-vs-test root-causing was
not done this run — flagged for a follow-up triage pass, not a v0.3.0 blocker.

- `test_connection.py::test_hello_returns_session_id`, `::test_pre_hello_rejects_other_verbs`, `::test_protocol_mismatch_on_wrong_major`, `::test_unmatched_quote_returns_invalid_args`
- `test_directory.py::test_directory_delete_non_empty_requires_recursive`
- `test_process.py::test_process_list_include_counters`, `::test_process_start_and_wait`, `::test_process_wait_after_exit_returns_cached_code`
- `test_vision.py::test_vision_ocr_path_source` (composite; tangled in the screen/region stale-suite issues above)
- `test_watch.py::test_watch_window_requires_title_prefix`, `::test_watch_process_returns_subscription_id`, `::test_watch_region_returns_subscription_id`

### E — Suite-internal fragility (non-deterministic)

`test_clipboard.py::test_clipboard_get_at_read_tier` passes in isolation but
fails inside the full run depending on suite ordering/clipboard state — a
canonical-suite coupling artifact, not agent code. May appear/disappear
between runs (27 vs 26 total depending on ordering).

## Fixed this run (no longer failing)

- `test_file.py::test_file_read_missing_returns_not_found`, `::test_file_write_missing_returns_not_found` — R4a added the existence probe → `not_found`.
- The unknown-flag rejection tests across input/keyboard/screen — R2's systemic `SchemaArgs` fix.

## Follow-up issues to file (out of scope this run)

1. Protocol-repo PR: fix the stale submodule tests in class **A** (clipboard.get shape, screen.capture image-content-item + region object, vision.ocr region, power.cancel tier) and teach `wire.py` to extract image content items.
2. `mcp-server/` bridge: expose `system.verbs` on the MCP tools surface (class **B**).
3. Triage pass for class **D** (agent-bug vs stale-test, per test).
4. `tests/conformance/fixtures/` is vestigial from the v2.0 fork (no resynced v2.2 test references it) — confirm and remove in a follow-up.
