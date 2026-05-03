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

"""Tests for `spec_loader`: resolution, parsing, x-* stripping, and the
CRUDX→tier / CRUDX→hints derivations."""

from __future__ import annotations

import pytest

from spec_loader import (
    crudx_to_hints,
    crudx_to_tier,
    load_specs,
    resolve_spec_dir,
    strip_x_extensions,
)


# ---------------------------------------------------------------------------
# Resolution and load

def test_resolve_spec_dir_finds_protocol_repo() -> None:
    """The default resolver walks up looking for `Protocol/spec/` or `spec/`.
    This succeeds on the developer layout where this test runs."""
    p = resolve_spec_dir()
    assert (p / "verbs").is_dir()
    assert (p / "families.json").is_file()


def test_load_specs_returns_all_19_verbs() -> None:
    specs = load_specs()
    expected = {
        "connection.hello",
        "system.info",
        "system.shutdown",
        "screen.capture",
        "window.list",
        "window.move",
        "input.click",
        "element.find",
        "file.read",
        "file.write",
        "file.delete",
        "file.download",
        "directory.create",
        "directory.list",
        "directory.stat",
        "directory.exists",
        "directory.rename",
        "directory.remove",
        "clipboard.get",
        "clipboard.set",
    }
    assert set(specs.keys()) == expected, \
        f"missing or extra: {expected ^ set(specs.keys())}"


def test_every_loaded_spec_carries_required_fields() -> None:
    specs = load_specs()
    for name, spec in specs.items():
        assert spec.get("name") == name, f"{name}: name mismatch"
        assert "input_schema" in spec, f"{name}: missing input_schema"
        assert "x-crudx" in spec, f"{name}: missing x-crudx"
        assert spec.get("strict") is True, f"{name}: not strict"
        assert spec["input_schema"].get("type") == "object", \
            f"{name}: input_schema is not an object"
        assert spec["input_schema"].get("additionalProperties") is False, \
            f"{name}: input_schema must have additionalProperties: false"


# ---------------------------------------------------------------------------
# x-* stripping

def test_strip_x_extensions_removes_top_level_x_keys() -> None:
    node = {"name": "x", "x-crudx": "R", "x-since": "2.0", "input_schema": {}}
    out = strip_x_extensions(node)
    assert "name" in out
    assert "input_schema" in out
    assert "x-crudx" not in out
    assert "x-since" not in out


def test_strip_x_extensions_recurses_into_nested_dicts_and_lists() -> None:
    node = {
        "outer": {
            "x-meta": "drop me",
            "kept": [
                {"x-inner": "drop me too", "value": 1},
                {"value": 2},
            ],
        },
        "x-top": "also drop",
    }
    out = strip_x_extensions(node)
    assert "x-top" not in out
    assert "x-meta" not in out["outer"]
    assert out["outer"]["kept"][0] == {"value": 1}
    assert out["outer"]["kept"][1] == {"value": 2}


def test_strip_x_extensions_leaves_non_x_keys_alone() -> None:
    node = {"x-foo": "drop", "foo": "keep", "extension": "keep — not prefixed"}
    out = strip_x_extensions(node)
    assert out == {"foo": "keep", "extension": "keep — not prefixed"}


def test_strip_x_extensions_handles_primitives() -> None:
    assert strip_x_extensions("hello") == "hello"
    assert strip_x_extensions(42) == 42
    assert strip_x_extensions(None) is None
    assert strip_x_extensions(True) is True


# ---------------------------------------------------------------------------
# CRUDX → tier mapping

def test_crudx_to_tier_known_letters() -> None:
    assert crudx_to_tier("R") == "read"
    assert crudx_to_tier("C") == "create"
    assert crudx_to_tier("U") == "update"
    assert crudx_to_tier("D") == "delete"
    assert crudx_to_tier("X") == "extra_risky"


def test_crudx_to_tier_unknown_raises() -> None:
    with pytest.raises(ValueError):
        crudx_to_tier("Q")
    with pytest.raises(ValueError):
        crudx_to_tier("")


# ---------------------------------------------------------------------------
# CRUDX → hints

def test_crudx_to_hints_read_only_for_R() -> None:
    spec = {"x-crudx": "R", "x-errors": []}
    h = crudx_to_hints(spec)
    assert h["read_only_hint"] is True
    assert h["destructive_hint"] is False
    assert h["idempotent_hint"] is True


def test_crudx_to_hints_destructive_for_D_and_X() -> None:
    for letter in ("D", "X"):
        spec = {"x-crudx": letter, "x-errors": []}
        h = crudx_to_hints(spec)
        assert h["read_only_hint"] is False
        assert h["destructive_hint"] is True
        assert h["idempotent_hint"] is False


def test_crudx_to_hints_neutral_for_C_and_U() -> None:
    for letter in ("C", "U"):
        spec = {"x-crudx": letter, "x-errors": []}
        h = crudx_to_hints(spec)
        assert h["read_only_hint"] is False
        assert h["destructive_hint"] is False
        assert h["idempotent_hint"] is False
