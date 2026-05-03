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

"""Loader for the protocol-repo's source-of-truth verb specs.

Reads `spec/verbs/*.json` and `spec/families.json` from the protocol repo and
exposes:
  - `load_specs(spec_dir)`: dict[verb_name -> spec dict].
  - `strip_x_extensions(node)`: removes `x-*` keys recursively (defensive
     boundary between the rich source files and the strict-tool definitions
     submitted to the Anthropic API).
  - `crudx_to_tier(letter)`: maps R/C/U/D/X to the wire-tier name.
  - `crudx_to_hints(spec)`: derives MCP `read_only_hint` /
     `destructive_hint` / `idempotent_hint` from CRUDX + `x-errors`.
  - `resolve_spec_dir()`: env var `PROTOCOL_SPEC_DIR` if set, else searches
     up from this file for `spec/verbs/`.

The loader runs at module import inside `tools.py`. Schema mutations to the
spec files in the protocol repo land here without bridge-side code changes —
the bridge is a thin shim that re-presents the strict-tool defs as MCP tools.
"""

from __future__ import annotations

import glob
import json
import os
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Defensive `x-*` stripping

def strip_x_extensions(node):
    """Recursively remove dict keys starting with `x-`. Used at the boundary
    between source spec files (which carry rich `x-*` metadata) and
    strict-tool definitions submitted to the Anthropic API (which the strict
    compiler may eventually reject unknown keys for). Standard OpenAPI-codegen
    pattern."""
    if isinstance(node, dict):
        return {k: strip_x_extensions(v) for k, v in node.items()
                if not k.startswith("x-")}
    if isinstance(node, list):
        return [strip_x_extensions(v) for v in node]
    return node


# ---------------------------------------------------------------------------
# CRUDX → tier mapping

# Five-rung ladder (per PROTOCOL.md §7). Values must match the wire vocabulary
# exactly — the tier names are sent verbatim in `connection.tier_raise`.
_CRUDX_TO_TIER = {
    "R": "read",
    "C": "create",
    "U": "update",
    "D": "delete",
    "X": "extra_risky",
}


def crudx_to_tier(letter: str) -> str:
    """Map a CRUDX letter (R/C/U/D/X) to the required wire tier."""
    try:
        return _CRUDX_TO_TIER[letter]
    except KeyError:
        raise ValueError(f"unknown CRUDX letter: {letter!r}")


def crudx_to_hints(spec: dict) -> dict:
    """Derive MCP `*_hint` annotation booleans from a verb spec.

    - `read_only_hint`: True for R verbs (no side effects on external state).
    - `destructive_hint`: True for D and X verbs.
    - `idempotent_hint`: True for R verbs by convention.
    """
    letter = spec.get("x-crudx", "")
    return {
        "read_only_hint": letter == "R",
        "destructive_hint": letter in ("D", "X"),
        "idempotent_hint": letter == "R",
    }


# ---------------------------------------------------------------------------
# Spec directory resolution

def resolve_spec_dir() -> Path:
    """Resolve the protocol-repo's `spec/` directory.

    Order:
      1. `PROTOCOL_SPEC_DIR` env var if set (must contain `verbs/`).
      2. Walk up from this file's parent looking for `<dir>/Protocol/spec`,
         then for `<dir>/spec` directly. Practical default for the
         current source layout: `<bridge>/../../Protocol/spec`.

    Raises `RuntimeError` with a clear message on miss.
    """
    env = os.environ.get("PROTOCOL_SPEC_DIR")
    if env:
        p = Path(env)
        if (p / "verbs").is_dir():
            return p
        raise RuntimeError(
            f"PROTOCOL_SPEC_DIR={env!r} does not contain a 'verbs/' subdirectory")

    here = Path(__file__).resolve().parent
    # Walk upward up to 5 levels looking for sibling layouts.
    candidates: list[Path] = []
    for up in range(5):
        base = here.parents[up] if up < len(here.parents) else None
        if base is None:
            break
        candidates.append(base / "Protocol" / "spec")
        candidates.append(base / "spec")
    for c in candidates:
        if (c / "verbs").is_dir():
            return c

    raise RuntimeError(
        "could not locate the protocol-repo spec/ directory. "
        "Set PROTOCOL_SPEC_DIR to the absolute path. Searched: " +
        ", ".join(str(c) for c in candidates))


# ---------------------------------------------------------------------------
# Spec loading

def load_specs(spec_dir: Optional[Path] = None) -> dict[str, dict]:
    """Load every `spec/verbs/*.json` and return `{verb_name: spec_dict}`.

    `spec_dir` defaults to `resolve_spec_dir()`. Raises on parse errors or
    on a verb file whose `name` field disagrees with its filename basename
    (a useful sanity check for hand-edited spec files)."""
    if spec_dir is None:
        spec_dir = resolve_spec_dir()
    verbs_dir = spec_dir / "verbs"
    out: dict[str, dict] = {}
    for path in sorted(glob.glob(str(verbs_dir / "*.json"))):
        with open(path, "r", encoding="utf-8") as f:
            spec = json.load(f)
        name = spec.get("name")
        if not name:
            raise RuntimeError(f"{path}: spec missing 'name' field")
        # Filename should match the verb name.
        expected_basename = name + ".json"
        if Path(path).name != expected_basename:
            raise RuntimeError(
                f"{path}: filename does not match spec name "
                f"(expected {expected_basename!r})")
        out[name] = spec
    return out
