#   Copyright 2026 William Isted and contributors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

"""Variant-family registry — hand-curated map of MCP tool name -> related
tools the LLM should consider as alternatives.

Three consumers:

1. `evaluate_with_variants` (test mode V3 forced-coverage): when an LLM
   calls this meta-tool with preferred + variants, the bridge checks every
   family member is addressed (either included as a variant, or documented
   in `ruled_out_reason`) and returns a `coverage_gap` field listing what
   the LLM didn't address.

2. (Future) Production loop-detection coaching: when an LLM calls the same
   tool repeatedly with failure-equivalent results, the bridge appends a
   `loop_advisory` field suggesting alternatives from this registry.

3. (Future, doc-time) `LLM-OPERATORS.md` decision tree: the `alternatives`
   reasons text doubles as the source for the operator-facing decision
   tree. Hand-edited but kept in sync.

This is the MCP-tool surface (what the LLM sees), NOT the raw wire-verb
surface. Tool names match those in `tools.py`'s ToolDef registry. When new
tools ship, update this file as part of the same PR.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List


@dataclass
class FamilyEntry:
    """One family. The `family` list is the set of tools the LLM should
    consider as alternatives when this tool would otherwise be the obvious
    pick. The `alternatives` map gives a one-sentence reason per member to
    be surfaced in coverage-gap advisories and feedback prompts."""

    family: List[str] = field(default_factory=list)
    self_caveats: str = ""
    alternatives: Dict[str, str] = field(default_factory=dict)


# Hand-curated registry. Keep entries small and focused on cases where
# multiple tools plausibly compete for the same task. Pure singletons
# (kill_process, cancel_pending_shutdown, etc.) are intentionally omitted
# — there's nothing for the LLM to consider as an alternative.

VARIANT_FAMILIES: Dict[str, FamilyEntry] = {

    # -----------------------------------------------------------------
    # Click — coordinate click vs UIA-element-id click
    # -----------------------------------------------------------------
    "click": FamilyEntry(
        family=["click_element"],
        self_caveats=(
            "coordinate click — fast and works on any visible target, but "
            "fragile if the layout shifts between find and click."
        ),
        alternatives={
            "click_element": (
                "click via UIA element id — more robust to layout changes "
                "since the element identity is preserved across renders. "
                "Prefer when the target is UIA-visible and you have an "
                "element id from find_element / element.at."
            ),
        },
    ),
    "click_element": FamilyEntry(
        family=["click"],
        self_caveats=(
            "click via UIA element id — robust but depends on UIA visibility "
            "of the target."
        ),
        alternatives={
            "click": (
                "coordinate click — required when the target isn't "
                "UIA-visible (canvas-rendered UI, some Unity / DirectComposition "
                "overlays). Use after take_screenshot to identify coordinates."
            ),
        },
    ),

    # -----------------------------------------------------------------
    # "Wait for something to happen" family
    # -----------------------------------------------------------------
    # These are alternatives when the LLM wants to block until an event
    # occurs. Each watches a different layer (UIA element, top-level
    # window, visual region, file, process exit). Pick based on what's
    # most diagnostic of the awaited event.

    "wait_for_element": FamilyEntry(
        family=["wait_for_window", "wait_for_visual_change", "find_element"],
        self_caveats=(
            "polling UIA find with timeout. Right when the awaited thing "
            "is a UIA-visible element. Won't fire for overlays UIA can't "
            "see (some Unity/canvas/DirectComposition content)."
        ),
        alternatives={
            "wait_for_window": (
                "wait for a top-level Win32 window with matching title "
                "prefix. Right when the awaited event is a separate "
                "window appearing (installer dialog, error popup)."
            ),
            "wait_for_visual_change": (
                "wait for any pixel change in a region. The fallback for "
                "non-UIA-visible content; fires on animation as well as "
                "real changes."
            ),
            "find_element": (
                "single-shot version. Use when the element should already "
                "be present; faster than wait_for_element."
            ),
        },
    ),
    "wait_for_window": FamilyEntry(
        family=["wait_for_element", "wait_for_visual_change", "find_window"],
        self_caveats=(
            "watches Win32 top-level windows. Won't fire for Unity / web "
            "/ canvas overlays painted inside an existing window — use "
            "wait_for_visual_change or wait_for_element for those."
        ),
        alternatives={
            "wait_for_element": (
                "wait for a UIA element by role + name pattern. Use when "
                "the awaited thing is inside an existing window."
            ),
            "wait_for_visual_change": (
                "wait for any pixel change in a region. Use for visual-only "
                "changes that aren't structurally surfaced."
            ),
            "find_window": (
                "single-shot window search. Use when you expect the window "
                "to already exist."
            ),
        },
    ),
    "wait_for_visual_change": FamilyEntry(
        family=["wait_for_element", "wait_for_window"],
        self_caveats=(
            "fires on any pixel change including animation; expensive in "
            "bandwidth (returns a PNG per fire). Prefer structural waits "
            "when the target is UIA-visible."
        ),
        alternatives={
            "wait_for_element": (
                "structural form. Cheaper and more semantically meaningful "
                "when the target is UIA-visible."
            ),
            "wait_for_window": (
                "use when the awaited event is a top-level Win32 window "
                "appearing — much cleaner than visual diffing."
            ),
        },
    ),
    "wait_for_process_exit": FamilyEntry(
        family=["wait_for_file_change"],
        self_caveats=(
            "watches a specific PID for exit. Returns the exit code. Right "
            "for installer / build / long-running-command flows."
        ),
        alternatives={
            "wait_for_file_change": (
                "use when waiting for the process to produce / modify a "
                "specific output file rather than waiting for it to exit."
            ),
        },
    ),
    "wait_for_file_change": FamilyEntry(
        family=["wait_for_file", "wait_for_process_exit"],
        self_caveats=(
            "fires on any modification to files matching the pattern "
            "(create / delete / modify / rename). Use for 'something "
            "wrote to this' rather than 'this exists'."
        ),
        alternatives={
            "wait_for_file": (
                "use when waiting for a path to *exist* (download / build "
                "artifact appears). Different from any-modification."
            ),
            "wait_for_process_exit": (
                "use when the process producing the file is one you "
                "launched and want to wait for completion of."
            ),
        },
    ),

    # -----------------------------------------------------------------
    # Find / locate UI — element vs window vs wait
    # -----------------------------------------------------------------
    "find_element": FamilyEntry(
        family=["find_window", "wait_for_element", "list_elements"],
        self_caveats=(
            "single-shot UIA element find — returns immediately. If the "
            "element isn't present yet, prefer wait_for_element."
        ),
        alternatives={
            "find_window": (
                "find a top-level Win32 window by title prefix. Wider scope; "
                "use when looking for a separate dialog window rather than "
                "an element within the current window."
            ),
            "wait_for_element": (
                "polling form of find_element with a timeout. Use when the "
                "element may not have rendered yet (e.g., right after "
                "clicking a button that opens a dialog)."
            ),
            "list_elements": (
                "enumerate all visible elements in a region. Use when you "
                "don't know the exact role/name of the target and want to "
                "see what's available."
            ),
        },
    ),
    "find_window": FamilyEntry(
        family=["find_element", "list_windows"],
        self_caveats=(
            "finds a top-level Win32 window by title prefix. Doesn't see "
            "in-window UI elements; use find_element for those."
        ),
        alternatives={
            "find_element": (
                "find a UIA element within the current foreground window. "
                "Use for buttons, text fields, and other in-window controls."
            ),
            "list_windows": (
                "enumerate all top-level windows. Use when you don't know "
                "the exact title prefix or want to see what's open."
            ),
        },
    ),
    "wait_for_element": FamilyEntry(
        family=["find_element", "wait_for_file"],
        self_caveats=(
            "polling form of find_element; periodic UIA tree probe with a "
            "timeout. Use when the element will appear soon but isn't "
            "there yet."
        ),
        alternatives={
            "find_element": (
                "single-shot version. Use when you expect the element to "
                "be present already; faster than wait_for_element for the "
                "common case."
            ),
            "wait_for_file": (
                "wait for a filesystem path to appear. Use when the LLM is "
                "waiting for a build artifact or download rather than a UI "
                "element."
            ),
        },
    ),

    # -----------------------------------------------------------------
    # Text input — typing vs UIA-direct-set vs key-by-key
    # -----------------------------------------------------------------
    "type_text": FamilyEntry(
        family=["set_element_text", "press_keys"],
        self_caveats=(
            "synthesises Unicode keystrokes via SendInput at the foreground "
            "window. Subject to focus stealing; some games / DirectInput "
            "targets ignore SendInput keystrokes (see PROTOCOL.md §4.4)."
        ),
        alternatives={
            "set_element_text": (
                "directly set a UIA element's text value via ValuePattern. "
                "More reliable than type_text for text fields with "
                "validation or auto-complete; bypasses focus issues."
            ),
            "press_keys": (
                "press named keys with optional modifiers. Use for "
                "single-key actions (Enter, Tab, F4) or shortcuts "
                "(Ctrl+S). Not for entering free-form text."
            ),
        },
    ),
    "set_element_text": FamilyEntry(
        family=["type_text"],
        self_caveats=(
            "directly sets a UIA element's text via ValuePattern. Requires "
            "the element to be UIA-visible and support ValuePattern."
        ),
        alternatives={
            "type_text": (
                "synthesise keystrokes — use for non-UIA-visible text "
                "fields (canvas-rendered, some game UI), or when the "
                "element rejects programmatic value-set but accepts "
                "typed input."
            ),
        },
    ),

    # -----------------------------------------------------------------
    # File write — text vs base64 vs upload from controller
    # -----------------------------------------------------------------
    "write_file": FamilyEntry(
        family=["write_file_b64", "upload_file"],
        self_caveats=(
            "UTF-8 text write only. Will mojibake any non-text content."
        ),
        alternatives={
            "write_file_b64": (
                "binary content the LLM has in hand as base64. Use for "
                "small generated binary fixtures or when the content was "
                "produced by another tool that returned base64."
            ),
            "upload_file": (
                "read from a path on the controller and push to the target. "
                "Use for any binary content that exists as a file on the "
                "controller (zip, exe, DLL, asset). Bytes don't pass "
                "through MCP / LLM context, so this scales to large files."
            ),
        },
    ),
    "write_file_b64": FamilyEntry(
        family=["write_file", "upload_file"],
        self_caveats=(
            "base64-encoded binary. Burns LLM tokens proportional to file "
            "size; impractical for large files."
        ),
        alternatives={
            "write_file": (
                "use for UTF-8 text content the LLM is generating in-context."
            ),
            "upload_file": (
                "use for binary content larger than a few KB or that already "
                "exists as a file on the controller. Bytes don't pass "
                "through MCP."
            ),
        },
    ),
    "upload_file": FamilyEntry(
        family=["write_file", "write_file_b64"],
        self_caveats=(
            "reads from controller filesystem; pushes to target. Source path "
            "must exist on the controller (where the bridge runs)."
        ),
        alternatives={
            "write_file": (
                "use for UTF-8 text content the LLM is generating in-context "
                "(no source file on the controller)."
            ),
            "write_file_b64": (
                "use for small binary content the LLM has in hand as base64 "
                "(no source file on the controller)."
            ),
        },
    ),

    # -----------------------------------------------------------------
    # Process start — CreateProcess vs ShellExecute
    # -----------------------------------------------------------------
    "launch": FamilyEntry(
        family=["shell_open"],
        self_caveats=(
            "CreateProcess with the given command line. The first token is "
            "the executable; subsequent tokens are args. Doesn't go through "
            "the shell, so no PATH-extension lookup or .reg-defined "
            "associations."
        ),
        alternatives={
            "shell_open": (
                "ShellExecuteEx with the default verb. Use for opening "
                "files / URLs in their associated app (e.g., 'open file.pdf' "
                "in Acrobat, 'start http://...' in browser). Also use for "
                "elevation via verb='runas'."
            ),
        },
    ),
    "shell_open": FamilyEntry(
        family=["launch"],
        self_caveats=(
            "ShellExecuteEx — opens via shell associations. Returns "
            "synchronously but the spawned process may detach (PID 0)."
        ),
        alternatives={
            "launch": (
                "CreateProcess — use when you need the spawned PID and "
                "want direct control over the command line. Doesn't apply "
                "shell verb associations."
            ),
        },
    ),
}


def lookup(tool_name: str) -> FamilyEntry:
    """Return the family entry for `tool_name`, or an empty entry if the
    tool has no registered alternatives. Empty-entry lookups are valid and
    indicate "no variants to coverage-check."""
    return VARIANT_FAMILIES.get(tool_name, FamilyEntry())


def has_family(tool_name: str) -> bool:
    """True if the tool has at least one registered alternative."""
    return tool_name in VARIANT_FAMILIES and bool(
        VARIANT_FAMILIES[tool_name].family)
