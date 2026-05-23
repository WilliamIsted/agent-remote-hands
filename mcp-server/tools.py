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

"""MCP tool surface for the Agent Remote Hands wire protocol.

Tools are tagged with the wire-protocol tier they require (the v2.1 CRUDX
ladder: read < create < update < delete < extra_risky). The server consults
that tag when answering `tools/list` so a `read`-tier session sees only the
read tools; the LLM raises tier explicitly via the `request_*_access` family,
which the server treats as an elevation request and (on success) emits a
`tools/list_changed` notification so the client re-queries and sees the
broader surface.

Tool naming: semantic over wire-mechanical (e.g. `click_element`, not
`element_invoke_at_xy`) per the agent CLAUDE.md guidance — the LLM reads
the tool description and decides intent from it.

Schema source-of-truth: tools whose wire verb has a corresponding entry under
the protocol-repo's `spec/verbs/<verb>.json` load their `input_schema` from
the spec file via `ToolDef.from_spec(...)`. Tools without a spec equivalent
(composite dispatchers, watch-based wait tools, bridge-internal helpers) keep
their inline `input_schema=...` definitions. Set `PROTOCOL_SPEC_DIR` to point
the loader at a non-default spec/ location.
"""

from __future__ import annotations

import base64
import json
import os
import socket
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from agent_client import AgentClient, ErrResponse, OkResponse, WireError, read_token_file
from spec_loader import (
    crudx_to_hints,
    crudx_to_tier,
    load_specs,
    strip_x_extensions,
)


# ---------------------------------------------------------------------------
# Spec-of-truth load
#
# The 15-verb spec lives in the protocol repo at `Repos/Protocol/spec/`.
# Loaded once at module import; lifted ToolDefs read from this dict.

SPECS: dict[str, dict] = load_specs()


# ---------------------------------------------------------------------------
# Tool registry types

# The valid tier values, ordered as a strict ladder. Matches the agent's
# Tier enum (PROTOCOL.md §7). `always` is bridge-internal — used for tools
# that should appear regardless of the connection's current tier (e.g. the
# `system.info` and tier-elevation tools).
_TIER_ORDER = {
    "always":      -1,
    "read":         0,
    "create":       1,
    "update":       2,
    "delete":       3,
    "extra_risky":  4,
}


@dataclass
class ToolDef:
    name: str
    description: str
    tier: str  # "read" | "create" | "update" | "delete" | "extra_risky" | "always"
    input_schema: dict
    handler: Callable[[dict, AgentClient], str]
    # MCP standard annotations — the client uses these for safety prompting.
    read_only_hint: bool = False
    destructive_hint: bool = False
    idempotent_hint: bool = False
    open_world_hint: bool = False
    annotations: dict = field(default_factory=dict)
    # Optional: the wire verb this MCP tool dispatches to. For diagnostics —
    # handlers still call `client.request("<verb>", ...)` explicitly. Set
    # automatically when constructed via `ToolDef.from_spec`.
    wire_verb: Optional[str] = None

    def to_mcp_tool(self) -> dict:
        """Serialise into the MCP `Tool` schema. The tier field is internal
        to this server and is not part of the MCP wire shape — clients see
        the standard annotations for safety hinting."""
        ann: dict[str, Any] = {
            "readOnlyHint": self.read_only_hint,
            "destructiveHint": self.destructive_hint,
            "idempotentHint": self.idempotent_hint,
            "openWorldHint": self.open_world_hint,
        }
        ann.update(self.annotations)
        return {
            "name": self.name,
            "description": self.description,
            "inputSchema": self.input_schema,
            "annotations": ann,
        }

    @classmethod
    def from_spec(cls,
                  spec_verb_name: str,
                  mcp_name: str,
                  handler: Callable[[dict, AgentClient], str],
                  *,
                  description: Optional[str] = None,
                  tier: Optional[str] = None,
                  wire_verb: Optional[str] = None,
                  extra_annotations: Optional[dict] = None) -> "ToolDef":
        """Build a ToolDef from a spec verb file. The lifted MCP tool's
        `input_schema` is the spec's `input_schema` with `x-*` extensions
        stripped; `tier` defaults to the CRUDX-derived value; the standard
        MCP annotation hints are derived from the CRUDX letter and `x-errors`.

        - `description`: an MCP-side override (preferred when richer than
          the spec's terse description). Falls back to spec `description`.
        - `tier`: explicit override, e.g. `"always"` for `system.info`.
        - `wire_verb`: the wire verb to dispatch to, when it differs from
          the spec name. Stored as documentation; handlers still call
          `client.request(...)` with the wire name explicitly.
        - `extra_annotations`: merged into the MCP annotations dict.
        """
        try:
            spec = SPECS[spec_verb_name]
        except KeyError:
            raise RuntimeError(
                f"ToolDef.from_spec: no spec for {spec_verb_name!r}; "
                f"loaded specs: {sorted(SPECS)}")
        hints = crudx_to_hints(spec)
        return cls(
            name=mcp_name,
            description=description or spec.get("description", ""),
            tier=tier or crudx_to_tier(spec["x-crudx"]),
            input_schema=strip_x_extensions(spec["input_schema"]),
            handler=handler,
            read_only_hint=hints["read_only_hint"],
            destructive_hint=hints["destructive_hint"],
            idempotent_hint=hints["idempotent_hint"],
            annotations=extra_annotations or {},
            wire_verb=wire_verb or spec_verb_name,
        )


# ---------------------------------------------------------------------------
# Wire-call helpers

def _ok_or_raise(r, verb: str) -> OkResponse:
    if isinstance(r, ErrResponse):
        raise RuntimeError(f"{verb}: ERR {r.code} {json.dumps(r.detail)}")
    return r


def _format_ok(r: OkResponse) -> str:
    """Format an OK response for return as MCP TextContent. JSON bodies are
    pretty-printed; binary bodies are base64'd with a clear marker."""
    if not r.payload:
        return "OK"
    try:
        obj = json.loads(r.payload)
        return json.dumps(obj, indent=2)
    except json.JSONDecodeError:
        return f"<binary {len(r.payload)} bytes, base64>\n" + base64.b64encode(r.payload).decode("ascii")


# ---------------------------------------------------------------------------
# Tool handlers
#
# Each handler takes (args: dict, client: AgentClient) and returns the text
# the LLM sees. Handlers raise on failure; the server formats exceptions as
# MCP error responses.


# --- Always-available --------------------------------------------------------

def _h_agent_info(args: dict, client: AgentClient) -> str:
    info = client.info()
    return json.dumps(info, indent=2)


def _elevate(client: AgentClient, target_tier: str, reason: str) -> str:
    """Common machinery for the per-tier `request_*_access` handlers.

    Reads the agent's elevation token (if not already set on the client),
    then issues `connection.tier_raise <target_tier>`. Returns a message
    describing the new tier, or raises if the tier_raise fails."""
    if not reason.strip():
        raise ValueError(
            f"reason is required — explain why {target_tier} tier is needed")

    if client.can_satisfy(target_tier):
        return (f"Already at {client.current_tier} tier; "
                f"{target_tier}-tier tools are available.")

    token = client._token or read_token_file(
        os.environ.get("REMOTE_HANDS_TOKEN_PATH"))
    if token is None:
        raise RuntimeError(
            "no token available — set REMOTE_HANDS_TOKEN_PATH to point at "
            "the agent's token file (default "
            "%ProgramData%\\AgentRemoteHands\\token)")
    client.set_token(token)

    r = client.tier_raise(target_tier)
    if isinstance(r, ErrResponse):
        raise RuntimeError(
            f"tier_raise {target_tier} failed: {r.code} {json.dumps(r.detail)}")
    return (f"Elevated to {target_tier} tier (reason: {reason}). "
            f"{target_tier}-tier tools will appear on the next tools/list query.")


def _h_agent_reset(args: dict, client: AgentClient) -> str:
    """Manual escape-hatch recovery primitive. Tears down the agent
    connection and re-establishes it, returning a structured summary.

    Bridge-internal: not a wire verb. The v2.2 spec deliberately does
    not expose `connection.reset` over MCP, and the failure modes this
    addresses (bridge-side `_buf` desync, half-dead socket after a
    framing error, agent process restarted under us) all live on the
    bridge side anyway.

    The auto-recovery wrapper in server.py handles read-only verbs
    transparently. This manual tool is the LLM-callable fallback when
    auto-recovery doesn't apply (non-idempotent verb that failed
    mid-call) or itself failed."""
    reason = args.get("reason", "").strip()
    if not reason:
        raise ValueError(
            "reason is required — explain what failure pattern prompted "
            "the reset (e.g. 'wire_error on file.write retry')")
    result = client.reset()
    return json.dumps({"reason": reason, **result}, indent=2)


def _h_request_create_access(args: dict, client: AgentClient) -> str:
    return _elevate(client, "create", args.get("reason", ""))


def _h_request_update_access(args: dict, client: AgentClient) -> str:
    return _elevate(client, "update", args.get("reason", ""))


def _h_request_delete_access(args: dict, client: AgentClient) -> str:
    return _elevate(client, "delete", args.get("reason", ""))


def _h_request_extra_risky_access(args: dict, client: AgentClient) -> str:
    return _elevate(client, "extra_risky", args.get("reason", ""))


# --- Observe -----------------------------------------------------------------

def _h_take_screenshot(args: dict, client: AgentClient) -> str:
    """Screen capture. v2.2 input shape: `region` is `{x, y, w, h}` object,
    `monitor` is integer index, `window` is handle string. Mutually
    exclusive. The agent returns the encoded image bytes as an MCP image
    content item; the wire client decodes the base64 payload into raw
    bytes on r.payload."""
    fmt = args.get("format", "png")
    arguments: dict = {"format": fmt}
    region = args.get("region")
    if region is not None:
        if isinstance(region, dict):
            arguments["region"] = {
                "x": int(region["x"]), "y": int(region["y"]),
                "w": int(region["w"]), "h": int(region["h"]),
            }
        elif isinstance(region, str):
            # Tolerate legacy "x,y,w,h" string form from older clients.
            parts = [int(p) for p in region.split(",")]
            arguments["region"] = {
                "x": parts[0], "y": parts[1], "w": parts[2], "h": parts[3],
            }
    if args.get("window") is not None:
        arguments["window"] = str(args["window"])
    if args.get("monitor") is not None:
        arguments["monitor"] = int(args["monitor"])
    r = _ok_or_raise(
        client.request_args("screen.capture", arguments), "screen.capture")
    # Don't dump the bytes inline — return a compact marker the LLM understands.
    return (f"Captured {len(r.payload)} bytes ({fmt}). To view, save the payload "
            f"to a file or pipe through an image-viewer. Use region/window/monitor "
            f"to crop and shrink the response.")


def _h_list_windows(args: dict, client: AgentClient) -> str:
    """List top-level windows. v2.2 input: `visible_only`, `pid`,
    `pattern`, `include_monitor`, `limit`, `full`."""
    arguments: dict = {}
    if args.get("visible_only") is not None:
        arguments["visible_only"] = bool(args["visible_only"])
    if args.get("pid") is not None:
        arguments["pid"] = int(args["pid"])
    if args.get("pattern") is not None:
        arguments["pattern"] = str(args["pattern"])
    if args.get("include_monitor") is not None:
        arguments["include_monitor"] = bool(args["include_monitor"])
    if args.get("limit") is not None:
        arguments["limit"] = int(args["limit"])
    if args.get("full") is not None:
        arguments["full"] = bool(args["full"])
    return _format_ok(_ok_or_raise(
        client.request_args("window.list", arguments), "window.list"))


def _h_find_window(args: dict, client: AgentClient) -> str:
    """v2.2 window.find takes `pattern` (substring by default) + optional
    `match` (substring / prefix / exact / glob / regex). The bridge accepts
    `title_pattern` from older callers as an alias for `pattern`."""
    pattern = args.get("pattern") or args.get("title_pattern")
    if not pattern:
        raise ValueError("window.find requires `pattern` (substring to match)")
    arguments: dict = {"pattern": str(pattern)}
    if args.get("match"):
        arguments["match"] = str(args["match"])
    return _format_ok(_ok_or_raise(
        client.request_args("window.find", arguments), "window.find"))


def _h_list_elements(args: dict, client: AgentClient) -> str:
    """element.list with optional `region` (now a {x,y,w,h} object). The
    bridge accepts the legacy "x,y,w,h" string form and converts."""
    arguments: dict = {}
    region = args.get("region")
    if region is not None:
        if isinstance(region, dict):
            arguments["region"] = {
                "x": int(region["x"]), "y": int(region["y"]),
                "w": int(region["w"]), "h": int(region["h"]),
            }
        elif isinstance(region, str):
            parts = [int(p) for p in region.split(",")]
            arguments["region"] = {
                "x": parts[0], "y": parts[1], "w": parts[2], "h": parts[3],
            }
    return _format_ok(_ok_or_raise(
        client.request_args("element.list", arguments), "element.list"))


def _h_find_element(args: dict, client: AgentClient) -> str:
    """Find a UIA element. v2.2 input: `role`, `name`, `automation_id`,
    `root` (window handle), `timeout_ms`. At least one of role / name /
    automation_id must be present."""
    arguments: dict = {}
    if args.get("role"):
        arguments["role"] = str(args["role"])
    name = args.get("name") or args.get("name_pattern")
    if name:
        arguments["name"] = str(name)
    if args.get("automation_id"):
        arguments["automation_id"] = str(args["automation_id"])
    if args.get("root"):
        arguments["root"] = str(args["root"])
    if args.get("timeout_ms") is not None:
        arguments["timeout_ms"] = int(args["timeout_ms"])
    if not (arguments.get("role") or arguments.get("name")
            or arguments.get("automation_id")):
        raise ValueError(
            "find_element requires at least one of `role`, `name`, "
            "or `automation_id`")
    return _format_ok(_ok_or_raise(
        client.request_args("element.find", arguments), "element.find"))


def _h_wait_for_element(args: dict, client: AgentClient) -> str:
    """element.wait: polling form of element.find. v2.2 input: `role`,
    `name`, `automation_id`, `root`, `flags_required`, `timeout_ms`."""
    arguments: dict = {}
    if args.get("role"):
        arguments["role"] = str(args["role"])
    name = args.get("name") or args.get("name_pattern")
    if name:
        arguments["name"] = str(name)
    if args.get("automation_id"):
        arguments["automation_id"] = str(args["automation_id"])
    if args.get("root"):
        arguments["root"] = str(args["root"])
    if args.get("flags_required") is not None:
        arguments["flags_required"] = list(args["flags_required"])
    arguments["timeout_ms"] = int(args.get("timeout_ms", 30000))
    return _format_ok(_ok_or_raise(
        client.request_args("element.wait", arguments), "element.wait"))


def _h_get_range_value(args: dict, client: AgentClient) -> str:
    """element.range_value (R8). v2.2 input: {handle}. Returns UIA
    RangeValuePattern's min/max/value plus optional small_change/
    large_change/readonly. Surfaces ERR not_supported_by_target when
    the element doesn't implement the pattern."""
    handle = args.get("handle")
    if not handle:
        raise ValueError("get_range_value requires `handle`")
    return _format_ok(_ok_or_raise(
        client.request_args("element.range_value", {"handle": str(handle)}),
        "element.range_value"))


def _h_get_element_text(args: dict, client: AgentClient) -> str:
    """element.get_text (R9). v2.2 input: {handle, max_length?, offset?}.
    Reads UIA TextPattern (ValuePattern / Name fallbacks). Returns
    {text, length, truncated, offset} so the caller can paginate or
    narrow the handle when length is large."""
    handle = args.get("handle")
    if not handle:
        raise ValueError("get_element_text requires `handle`")
    arguments: dict = {"handle": str(handle)}
    if args.get("max_length") is not None:
        arguments["max_length"] = int(args["max_length"])
    if args.get("offset") is not None:
        arguments["offset"] = int(args["offset"])
    return _format_ok(_ok_or_raise(
        client.request_args("element.get_text", arguments),
        "element.get_text"))


def _h_search_elements(args: dict, client: AgentClient) -> str:
    """element.search (R10). v2.2 input: {root, patterns, match?,
    case_sensitive?, context_chars?, max_hits_per_pattern?, include_bounds?}.
    Server-side multi-pattern search returning excerpts + handles +
    (opt-in) screen-rect bounds for drag-select workflows."""
    root = args.get("root")
    if not root:
        raise ValueError("search_elements requires `root` (element handle)")
    patterns = args.get("patterns")
    if not isinstance(patterns, list) or len(patterns) == 0:
        raise ValueError(
            "search_elements requires a non-empty `patterns` list")
    arguments: dict = {
        "root": str(root),
        "patterns": [str(p) for p in patterns],
    }
    if args.get("match"):
        arguments["match"] = str(args["match"])
    if args.get("case_sensitive") is not None:
        arguments["case_sensitive"] = bool(args["case_sensitive"])
    if args.get("context_chars") is not None:
        arguments["context_chars"] = int(args["context_chars"])
    if args.get("max_hits_per_pattern") is not None:
        arguments["max_hits_per_pattern"] = int(args["max_hits_per_pattern"])
    if args.get("include_bounds") is not None:
        arguments["include_bounds"] = bool(args["include_bounds"])
    return _format_ok(_ok_or_raise(
        client.request_args("element.search", arguments),
        "element.search"))


def _h_wait_for_file(args: dict, client: AgentClient) -> str:
    """file.wait: v2.2 input is `{glob, timeout_ms}` (renamed from `pattern`
    in v2.1). Bridge accepts either as input."""
    glob_pattern = args.get("glob") or args.get("pattern")
    if not glob_pattern:
        raise ValueError("file.wait requires `glob` (or legacy `pattern`)")
    arguments: dict = {
        "glob": str(glob_pattern),
        "timeout_ms": int(args.get("timeout_ms", 60000)),
    }
    return _format_ok(_ok_or_raise(
        client.request_args("file.wait", arguments), "file.wait"))


def _h_read_file(args: dict, client: AgentClient) -> str:
    """file.read: v2.2 input is `{path, encoding, offset, length}`. Response
    is a JSON object `{content, encoding, bytes_read, eof}`.

    KNOWN GAP: in the current v2.2 agent build the content side-channel is
    deferred (Phase 2b) — calls return ERR invalid_args with a
    `{"reason":"...deferred..."}` body. The bridge surfaces that ERR
    verbatim until the side-channel lands."""
    arguments: dict = {"path": args["path"]}
    if args.get("encoding"):
        arguments["encoding"] = str(args["encoding"])
    if args.get("offset") is not None:
        arguments["offset"] = int(args["offset"])
    if args.get("length") is not None:
        arguments["length"] = int(args["length"])
    r = _ok_or_raise(
        client.request_args("file.read", arguments), "file.read")
    # OK body is a JSON object; pretty-print it.
    try:
        obj = json.loads(r.payload)
        # If the agent inlined `content` as decoded text, surface it.
        if isinstance(obj, dict) and isinstance(obj.get("content"), str):
            return obj["content"]
        return json.dumps(obj, indent=2)
    except json.JSONDecodeError:
        return r.payload.decode("utf-8", "replace")


def _h_list_directory(args: dict, client: AgentClient) -> str:
    """directory.list. v2.2 input: `{path, recursive, pattern, limit,
    offset, sort, reverse, full}`."""
    arguments: dict = {"path": args["path"]}
    for k in ("recursive", "pattern", "limit", "offset",
              "sort", "reverse", "full"):
        if args.get(k) is not None:
            arguments[k] = args[k]
    return _format_ok(_ok_or_raise(
        client.request_args("directory.list", arguments), "directory.list"))


def _h_list_processes(args: dict, client: AgentClient) -> str:
    """process.list. v2.2 input: `{pattern, include_counters, limit,
    include_system}`. Accepts legacy `name_filter` as alias for
    `pattern`."""
    arguments: dict = {}
    pattern = args.get("pattern") or args.get("name_filter")
    if pattern:
        arguments["pattern"] = str(pattern)
    for k in ("include_counters", "limit", "include_system"):
        if args.get(k) is not None:
            arguments[k] = args[k]
    return _format_ok(_ok_or_raise(
        client.request_args("process.list", arguments), "process.list"))


def _h_get_clipboard(args: dict, client: AgentClient) -> str:
    """clipboard.get. v2.2 returns `{content, format}` JSON; we extract
    `content` for ergonomic display."""
    arguments: dict = {}
    if args.get("format"):
        arguments["format"] = str(args["format"])
    r = _ok_or_raise(
        client.request_args("clipboard.get", arguments), "clipboard.get")
    try:
        obj = json.loads(r.payload)
        if isinstance(obj, dict) and isinstance(obj.get("content"), str):
            return obj["content"]
        return json.dumps(obj, indent=2)
    except json.JSONDecodeError:
        return r.payload.decode("utf-8", "replace")


# --- Drive -------------------------------------------------------------------

def _h_focus_window(args: dict, client: AgentClient) -> str:
    """v2.2 window.focus takes `{handle}`; bridge accepts legacy `hwnd`."""
    handle = args.get("handle") or args.get("hwnd")
    if not handle:
        raise ValueError("window.focus requires `handle` (or legacy `hwnd`)")
    return _format_ok(_ok_or_raise(
        client.request_args("window.focus", {"handle": str(handle)}),
        "window.focus"))


def _h_close_window(args: dict, client: AgentClient) -> str:
    """v2.2 window.close takes `{handle}`; bridge accepts legacy `hwnd`."""
    handle = args.get("handle") or args.get("hwnd")
    if not handle:
        raise ValueError("window.close requires `handle` (or legacy `hwnd`)")
    return _format_ok(_ok_or_raise(
        client.request_args("window.close", {"handle": str(handle)}),
        "window.close"))


def _h_click_element(args: dict, client: AgentClient) -> str:
    """Click a UIA element. Three call shapes the LLM can pick:
       - `handle` (`element_id` accepted as legacy alias, e.g. `"elt:7"`
         from a prior element.find / element.list)
       - `role` + `name` (compound element.find_invoke)
       - `x` + `y` (compound element.at_invoke)
    """
    elt_handle = args.get("handle") or args.get("element_id")
    role = args.get("role")
    name = args.get("name") or args.get("name_pattern")
    x = args.get("x")
    y = args.get("y")

    if elt_handle:
        return _format_ok(_ok_or_raise(
            client.request_args("element.invoke",
                                {"handle": str(elt_handle)}),
            "element.invoke"))
    if role and name:
        arguments: dict = {"role": str(role), "name": str(name)}
        if args.get("automation_id"):
            arguments["automation_id"] = str(args["automation_id"])
        if args.get("root"):
            arguments["root"] = str(args["root"])
        if args.get("timeout_ms") is not None:
            arguments["timeout_ms"] = int(args["timeout_ms"])
        return _format_ok(_ok_or_raise(
            client.request_args("element.find_invoke", arguments),
            "element.find_invoke"))
    if x is not None and y is not None:
        return _format_ok(_ok_or_raise(
            client.request_args("element.at_invoke",
                                {"x": int(x), "y": int(y)}),
            "element.at_invoke"))
    raise ValueError(
        "click_element requires one of: handle (or element_id), "
        "role+name, or x+y")


def _h_set_element_text(args: dict, client: AgentClient) -> str:
    """element.set_text. v2.2 input: `{handle, text}` — both inline string
    fields (no payload side-channel)."""
    handle = args.get("handle") or args.get("element_id")
    if not handle:
        raise ValueError(
            "element.set_text requires `handle` (or legacy `element_id`)")
    return _format_ok(_ok_or_raise(
        client.request_args("element.set_text", {
            "handle": str(handle),
            "text": str(args["text"]),
        }),
        "element.set_text"))


def _h_type_text(args: dict, client: AgentClient) -> str:
    """Type Unicode text. v2.2 wire verb: `input.keyboard.type` with
    `{text}` inline (renamed from `input.type` in v2.1)."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.keyboard.type",
                            {"text": str(args["text"])}),
        "input.keyboard.type"))


def _h_press_keys(args: dict, client: AgentClient) -> str:
    """Press a named key. v2.2 wire verb: `input.keyboard.key` with
    `{vk, modifiers, duration_ms}` (renamed from `input.key` in v2.1).
    Modifiers is now an array. The bridge accepts a comma-separated
    string for back-compat and splits it."""
    vk = args.get("vk") or args.get("key")
    if not vk:
        raise ValueError(
            "input.key requires `vk` (or legacy `key`)")
    arguments: dict = {"vk": str(vk)}
    mods = args.get("modifiers")
    if mods is not None:
        if isinstance(mods, str):
            mods = [m.strip() for m in mods.split(",") if m.strip()]
        arguments["modifiers"] = list(mods)
    if args.get("duration_ms") is not None:
        arguments["duration_ms"] = int(args["duration_ms"])
    return _format_ok(_ok_or_raise(
        client.request_args("input.keyboard.key", arguments),
        "input.keyboard.key"))


def _h_click_at(args: dict, client: AgentClient) -> str:
    """Click at an absolute screen coordinate. v2.2 wire verb:
    `input.mouse.click` with `{x, y, button, double, triple, clicks,
    clicks_interval_ms, duration_ms, modifiers}` (renamed from
    `input.click` in v2.1)."""
    arguments: dict = {
        "x": int(args["x"]),
        "y": int(args["y"]),
    }
    if args.get("button"):
        arguments["button"] = str(args["button"])
    if args.get("double"):
        arguments["double"] = bool(args["double"])
    if args.get("triple"):
        arguments["triple"] = bool(args["triple"])
    if args.get("clicks") is not None:
        arguments["clicks"] = int(args["clicks"])
    if args.get("clicks_interval_ms") is not None:
        arguments["clicks_interval_ms"] = int(args["clicks_interval_ms"])
    if args.get("duration_ms") is not None:
        arguments["duration_ms"] = int(args["duration_ms"])
    mods = args.get("modifiers")
    if mods is not None:
        if isinstance(mods, str):
            mods = [m.strip() for m in mods.split(",") if m.strip()]
        arguments["modifiers"] = list(mods)
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.click", arguments),
        "input.mouse.click"))


def _h_move_mouse(args: dict, client: AgentClient) -> str:
    """input.mouse.move. Wire input: `{x, y, relative?}`. Pass-through."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.move", args),
        "input.mouse.move"))


def _h_press_mouse(args: dict, client: AgentClient) -> str:
    """input.mouse.press. Wire input: `{x, y, button?, modifiers?}`.
    Used WITH input.mouse.release for true press-move-release sequences."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.press", args),
        "input.mouse.press"))


def _h_release_mouse(args: dict, client: AgentClient) -> str:
    """input.mouse.release. Wire input: `{x, y, button?, modifiers?}`."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.release", args),
        "input.mouse.release"))


def _h_drag_mouse(args: dict, client: AgentClient) -> str:
    """input.mouse.drag. Wire input: `{x, y, button?, steps?}`. Press at
    CURRENT cursor position, smoothly move to (x, y), release.

    For drag-from-A-to-B: first call input.mouse.move with A's coords,
    then input.mouse.drag with B's coords. This is the correct tool for
    drag-selecting text (e.g. Wikipedia quotes), dragging sliders,
    drawing selection rectangles."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.drag", args),
        "input.mouse.drag"))


def _h_scroll_mouse(args: dict, client: AgentClient) -> str:
    """input.mouse.scroll. Wire input varies by family; pass-through."""
    return _format_ok(_ok_or_raise(
        client.request_args("input.mouse.scroll", args),
        "input.mouse.scroll"))


def _h_write_file(args: dict, client: AgentClient) -> str:
    """Write UTF-8 text content. v2.2 input shape: `{path, content,
    encoding, atomic}` (all inline; no payload side-channel).

    KNOWN GAP: in the current v2.2 agent build file.write's `content`
    decode is deferred to Phase 2b — calls return
    `ERR invalid_args` with a `{"reason":"...deferred..."}` body. The
    bridge sends the schema-correct shape; the ERR is surfaced verbatim
    until Phase 2b lands. As a workaround for new files, prefer the
    bridge tool that maps to `file.create` (single-shot, no deferral) —
    not yet wrapped here, see TODO."""
    arguments = {
        "path": args["path"],
        "content": args["content"],
    }
    if args.get("encoding"):
        arguments["encoding"] = str(args["encoding"])
    if args.get("atomic") is not None:
        arguments["atomic"] = bool(args["atomic"])
    return _format_ok(_ok_or_raise(
        client.request_args("file.write", arguments),
        "file.write"))


def _h_write_file_b64(args: dict, client: AgentClient) -> str:
    """Write binary content the LLM has in hand as a base64 string. v2.2
    input shape: `{path, content (base64), encoding: "binary"}`. Same
    Phase-2b deferral note as `file.write` — the bridge sends the correct
    shape; the agent returns ERR until the side-channel lands."""
    path = args["path"]
    # Validate input is real base64.
    try:
        base64.b64decode(args["content_b64"], validate=True)
    except (ValueError, TypeError) as e:
        raise ValueError(f"content_b64 is not valid base64: {e}") from e
    return _format_ok(_ok_or_raise(
        client.request_args("file.write", {
            "path": path,
            "content": args["content_b64"],
            "encoding": "binary",
        }),
        "file.write"))


# Chunked-upload tunables. Files at or below SINGLE_SHOT_THRESHOLD use a
# single `file.write` (simpler, one round-trip). Larger files chunk via
# `file.write_at` with the parameters below. On chunk timeout / network
# error: 3 retries with exponential backoff, then halve the chunk size and
# try again. Bottoms out at MIN_CHUNK before reporting upload failure.
_UPLOAD_SINGLE_SHOT_THRESHOLD = 16 * 1024 * 1024   # 16 MB
_UPLOAD_INITIAL_CHUNK         = 16 * 1024 * 1024   # 16 MB starting chunk
_UPLOAD_MIN_CHUNK             = 256 * 1024         # 256 KB floor
_UPLOAD_MAX_RETRIES_PER_CHUNK = 3
_UPLOAD_BACKOFF_CAP_S         = 5.0


def _send_upload_chunk(client: AgentClient, dest: str, offset: int,
                       chunk: bytes, truncate: bool) -> None:
    """Send a single `file.write_at` request. v2.2 input shape:
    `{path, offset, content (base64), encoding: "binary", truncate}`.
    Raises on protocol-level failure (ErrResponse). KNOWN GAP: content
    decode is deferred (Phase 2b) — chunked upload is non-functional
    against the current agent build."""
    arguments = {
        "path": dest,
        "offset": offset,
        "content": base64.b64encode(chunk).decode("ascii"),
        "encoding": "binary",
    }
    if truncate:
        arguments["truncate"] = True
    r = client.request_args("file.write_at", arguments)
    if isinstance(r, ErrResponse):
        raise WireError(
            f"file.write_at[{offset}+{len(chunk)}]: {r.code} {r.detail}")


def _h_upload_file(args: dict, client: AgentClient) -> str:
    """Read a file from the controller's filesystem and write it to the
    target. Bytes flow controller-FS → bridge → wire → agent → target-FS,
    never through the MCP transport / LLM context. Suited for binary
    distribution (zip, exe, DLLs, archives, game assets) and for files of
    any size — chunks internally over the wire when the source exceeds
    16 MB.

    Fault tolerance: on per-chunk transport failure (timeout, connection
    reset), retries up to 3× with exponential backoff. If a chunk size
    keeps timing out, halves the chunk size and retries from the same
    offset. Bottoms out at 256 KB before reporting upload failure with
    progress detail.

    The `source_path` is read from wherever the bridge process runs —
    typically the same machine as the LLM client. The `destination_path`
    is interpreted by the agent on the target host."""
    source_path = args["source_path"]
    destination_path = args["destination_path"]

    try:
        size = os.path.getsize(source_path)
    except OSError as e:
        raise ValueError(f"could not read {source_path!r}: {e}") from e

    # Single-shot path for small files.
    if size <= _UPLOAD_SINGLE_SHOT_THRESHOLD:
        try:
            with open(source_path, "rb") as f:
                payload = f.read()
        except OSError as e:
            raise ValueError(f"could not read {source_path!r}: {e}") from e
        _ok_or_raise(
            client.request_args("file.write", {
                "path": destination_path,
                "content": base64.b64encode(payload).decode("ascii"),
                "encoding": "binary",
            }),
            "file.write")
        return f"uploaded {size} bytes to {destination_path} (single-shot)"

    # Chunked path for larger files.
    chunk_size = _UPLOAD_INITIAL_CHUNK
    offset = 0
    truncate_next = True
    chunks_sent = 0
    chunk_size_changes: list[tuple[int, int]] = []  # [(offset, new_size), ...]

    try:
        f = open(source_path, "rb")
    except OSError as e:
        raise ValueError(f"could not open {source_path!r}: {e}") from e

    try:
        while offset < size:
            f.seek(offset)
            chunk = f.read(min(chunk_size, size - offset))
            if not chunk:
                break  # shouldn't happen given the size check above

            sent = False
            last_err: Optional[Exception] = None
            for attempt in range(_UPLOAD_MAX_RETRIES_PER_CHUNK):
                try:
                    _send_upload_chunk(client, destination_path,
                                       offset, chunk, truncate_next)
                    sent = True
                    break
                except (TimeoutError, socket.timeout, OSError,
                        WireError) as e:
                    last_err = e
                    if attempt < _UPLOAD_MAX_RETRIES_PER_CHUNK - 1:
                        backoff = min(2 ** attempt, _UPLOAD_BACKOFF_CAP_S)
                        time.sleep(backoff)

            if sent:
                offset += len(chunk)
                truncate_next = False
                chunks_sent += 1
                continue

            # All retries exhausted at this chunk size. Halve and retry the
            # same offset with a smaller chunk if there's headroom.
            if chunk_size > _UPLOAD_MIN_CHUNK:
                new_size = max(_UPLOAD_MIN_CHUNK, chunk_size // 2)
                chunk_size_changes.append((offset, new_size))
                chunk_size = new_size
                continue

            # No more headroom. Report what we know.
            raise RuntimeError(
                f"upload failed at offset {offset}/{size} "
                f"(chunk_size={chunk_size}, chunks_sent={chunks_sent}, "
                f"last error: {last_err})"
            )
    finally:
        f.close()

    detail = f"uploaded {size} bytes to {destination_path} in {chunks_sent} chunks"
    if chunk_size_changes:
        detail += f"; reduced chunk size at offsets {chunk_size_changes}"
    return detail


def _h_launch(args: dict, client: AgentClient) -> str:
    """process.start. v2.2 input shape: `{argv (array), stdin, cwd}`. Bridge
    accepts legacy `command_line` (single string) and shlex-splits — caller
    can pass `argv` directly to bypass that."""
    arguments: dict = {}
    if args.get("argv") is not None:
        arguments["argv"] = list(args["argv"])
    elif args.get("command_line"):
        # shlex split would mishandle Windows paths with backslashes; use a
        # simple whitespace split. Callers wanting precise control should
        # pass `argv` directly.
        import shlex
        try:
            arguments["argv"] = shlex.split(args["command_line"], posix=False)
        except ValueError as e:
            raise ValueError(
                f"could not split command_line into argv: {e}") from e
    else:
        raise ValueError(
            "process.start requires `argv` (array) "
            "or legacy `command_line` (string)")
    if args.get("stdin") is not None:
        arguments["stdin"] = str(args["stdin"])
    if args.get("cwd") is not None:
        arguments["cwd"] = str(args["cwd"])
    return _format_ok(_ok_or_raise(
        client.request_args("process.start", arguments), "process.start"))


def _h_shell_open(args: dict, client: AgentClient) -> str:
    """process.shell. v2.2 input shape: `{path, args, verb, cwd}`."""
    arguments: dict = {"path": str(args["path"])}
    if args.get("args") is not None:
        arguments["args"] = str(args["args"])
    if args.get("verb"):
        arguments["verb"] = str(args["verb"])
    if args.get("cwd"):
        arguments["cwd"] = str(args["cwd"])
    return _format_ok(_ok_or_raise(
        client.request_args("process.shell", arguments), "process.shell"))


# --- Power -------------------------------------------------------------------

def _h_kill_process(args: dict, client: AgentClient) -> str:
    """process.kill. v2.2 input shape: `{pid, exit_code}`."""
    arguments: dict = {"pid": int(args["pid"])}
    if args.get("exit_code") is not None:
        arguments["exit_code"] = int(args["exit_code"])
    return _format_ok(_ok_or_raise(
        client.request_args("process.kill", arguments), "process.kill"))


def _h_delete_file(args: dict, client: AgentClient) -> str:
    """file.delete. v2.2 input shape: `{path}` (files only — use
    directory.delete for directories; the legacy `--recursive` flag is
    a directory.* concern in v2.2)."""
    arguments = {"path": str(args["path"])}
    return _format_ok(_ok_or_raise(
        client.request_args("file.delete", arguments), "file.delete"))


def _h_cancel_pending_shutdown(args: dict, client: AgentClient) -> str:
    return _format_ok(_ok_or_raise(
        client.request_args("system.power.cancel", {}),
        "system.power.cancel"))


# --- Subscription wait-fors (fire-once) --------------------------------------
#
# These tools translate one of the agent's `watch.*` subscription verbs into a
# synchronous MCP tool call. The flow is:
#
#   1. Send watch.X over the bridge's connection — agent returns OK + sub_id.
#   2. Block on the same connection waiting for the first matching EVENT
#      frame, OR until the user-supplied timeout elapses.
#   3. Send watch.cancel sub_id (idempotent — if the watch was --until-change
#      the agent has already auto-cancelled; this just makes sure we clean up
#      on timeout / failure paths).
#   4. Return a structured tool result: triggered + event body, or timeout.
#
# Continuous-subscription scenarios (LLM wants every change, not just the
# first) are not yet supported; if they're needed later, add a sub_id-returning
# tool plus a separate poll/cancel pair.

def _watch_subscription(client: AgentClient, verb: str,
                        wire_args: dict, timeout_ms: int,
                        sub_id_in_result: bool = True) -> tuple:
    """Common machinery for the wait-for-* tools. Subscribes via `verb` with
    a schema-correct arguments dict, waits up to `timeout_ms` for the first
    matching ``notifications/arh/event`` MCP notification, cancels, returns
    a (dict, event_body) pair suitable for inclusion in the tool result.

    In v2.2 EVENTs travel as MCP JSON-RPC notifications. The AgentClient
    adapter's :meth:`wait_for_event` strips the notification envelope and
    returns the inner ``params.data`` as bytes (JSON-encoded for object
    events; base64-decoded raw bytes for binary watch.region payloads)."""
    r = client.request_args(verb, wire_args)
    if isinstance(r, ErrResponse):
        raise WireError(f"{verb} failed: {r.code} {r.detail}")
    sub_id: Optional[str] = None
    try:
        sub_id = r.json().get("subscription_id")
    except Exception:
        pass
    if not sub_id:
        raise WireError(f"{verb} returned OK with no subscription_id: {r}")

    timed_out = False
    event_body: Optional[bytes] = None
    try:
        event_body = client.wait_for_event(sub_id, timeout_ms / 1000.0)
        timed_out = event_body is None
    finally:
        # Best-effort cancel. Auto-cancel watches are a no-op here; on
        # timeout / error this frees the agent-side worker.
        try:
            client.request_args(
                "watch.cancel", {"subscription_id": sub_id})
        except Exception:
            pass

    out: dict = {"subscription_id": sub_id, "timeout_ms": timeout_ms,
                 "timed_out": timed_out}
    if not sub_id_in_result:
        out.pop("subscription_id", None)
    return out, event_body


def _h_wait_for_visual_change(args: dict, client: AgentClient) -> str:
    """Wait for visual change in a screen region. Wraps
    ``watch.region`` with ``until_change: true``. Returns the captured
    frame as a base64-encoded PNG when the change fires, or a timeout
    marker.

    v2.2 input shape: `{region: {x,y,w,h}, interval_ms, until_change,
    encoding}`. Accepts the legacy "x,y,w,h" string form."""
    region = args["region"]
    if isinstance(region, str):
        parts = [int(p) for p in region.split(",")]
        region_obj = {"x": parts[0], "y": parts[1],
                      "w": parts[2], "h": parts[3]}
    else:
        region_obj = {
            "x": int(region["x"]), "y": int(region["y"]),
            "w": int(region["w"]), "h": int(region["h"]),
        }
    interval_ms = int(args.get("interval_ms", 500))
    timeout_ms = int(args.get("timeout_ms", 30000))

    out, body = _watch_subscription(
        client, "watch.region",
        {"region": region_obj,
         "interval_ms": interval_ms,
         "until_change": True},
        timeout_ms,
    )
    if out["timed_out"]:
        out["kind"] = "wait_for_visual_change_timeout"
    else:
        out["kind"] = "wait_for_visual_change_triggered"
        out["image_format"] = "png"
        out["image_size"] = len(body) if body else 0
        out["image_b64"] = base64.b64encode(body or b"").decode("ascii")
    return json.dumps(out)


def _h_wait_for_window(args: dict, client: AgentClient) -> str:
    """Wait for a top-level Win32 window matching a title prefix to
    appear (or disappear). Wraps ``watch.window``. v2.2 input shape:
    `{title_prefix}`."""
    title_prefix = args["title_prefix"]
    timeout_ms = int(args.get("timeout_ms", 30000))

    out, body = _watch_subscription(
        client, "watch.window",
        {"title_prefix": title_prefix},
        timeout_ms,
    )
    if out["timed_out"]:
        out["kind"] = "wait_for_window_timeout"
    else:
        out["kind"] = "wait_for_window_triggered"
        try:
            out["event"] = json.loads(body) if body else {}
        except (json.JSONDecodeError, TypeError):
            out["event_raw"] = (body or b"").decode("utf-8", "replace")
    return json.dumps(out)


def _h_wait_for_process_exit(args: dict, client: AgentClient) -> str:
    """Wait for a process to exit. Wraps ``watch.process`` (auto-cancels
    on exit). v2.2 input shape: `{pid}`."""
    timeout_ms = int(args.get("timeout_ms", 60000))

    out, body = _watch_subscription(
        client, "watch.process",
        {"pid": int(args["pid"])},
        timeout_ms,
    )
    if out["timed_out"]:
        out["kind"] = "wait_for_process_exit_timeout"
    else:
        out["kind"] = "wait_for_process_exit_triggered"
        try:
            out["event"] = json.loads(body) if body else {}
        except (json.JSONDecodeError, TypeError):
            out["event_raw"] = (body or b"").decode("utf-8", "replace")
    return json.dumps(out)


def _h_wait_for_file_change(args: dict, client: AgentClient) -> str:
    """Wait for any file modification matching a glob pattern. Wraps
    ``watch.file``. v2.2 input shape: `{glob, recursive}`. Bridge accepts
    legacy `pattern` as an alias for `glob`."""
    glob_pattern = args.get("glob") or args.get("pattern")
    if not glob_pattern:
        raise ValueError(
            "wait_for_file_change requires `glob` (or legacy `pattern`)")
    timeout_ms = int(args.get("timeout_ms", 60000))

    wire_args: dict = {"glob": str(glob_pattern)}
    if args.get("recursive") is not None:
        wire_args["recursive"] = bool(args["recursive"])
    out, body = _watch_subscription(
        client, "watch.file", wire_args, timeout_ms,
    )
    if out["timed_out"]:
        out["kind"] = "wait_for_file_change_timeout"
    else:
        out["kind"] = "wait_for_file_change_triggered"
        try:
            out["event"] = json.loads(body) if body else {}
        except (json.JSONDecodeError, TypeError):
            out["event_raw"] = (body or b"").decode("utf-8", "replace")
    return json.dumps(out)


# --- Test-mode meta-tool: evaluate_with_variants -----------------------------

def _h_evaluate_with_variants(args: dict, client: AgentClient) -> str:
    """Test-mode meta-tool. Runs a preferred tool call plus a list of
    variant tool calls; returns combined results plus a coverage-gap field
    listing registry-known alternatives the LLM didn't address.

    Each variant entry is either:
      - {"tool": "...", "arguments": {...}}                      → runs
      - {"tool": "...", "arguments": {...},
         "ruled_out_reason": "..."}                              → skipped, logged

    The ruled_out_reason form is how the LLM documents why it's NOT running
    a variant. The bridge counts it as 'addressed' for coverage purposes
    while still surfacing the LLM's reasoning in the response.

    Coverage gap: if the preferred tool is in `variant_families.VARIANT_FAMILIES`
    and any registry-listed alternative isn't addressed (neither in variants
    nor in ruled_out), the response includes a `coverage_gap` field with the
    missing tools and bridge-suggested reasons text. The LLM should either
    include the missing variants on its next call, or document them as
    ruled_out_reason.

    Outside test mode this tool is still callable but the response shape is
    the same — the meta-tool doesn't change behaviour based on a flag, the
    LLM just doesn't have to call it.

    See `Documents/Agent Remote Hands/To Do/test-mode-evaluation-loop-notes.md`
    for the full design rationale.
    """
    import time
    from variant_families import lookup, has_family

    preferred = args["preferred"]
    variants = args.get("variants", [])

    if "tool" not in preferred or "arguments" not in preferred:
        raise ValueError(
            "preferred must be an object with 'tool' (string) and "
            "'arguments' (object); got " + repr(preferred))

    # Validate the preferred tool exists BEFORE running. Unknown-tool here
    # is an input error worth raising loudly; the LLM should fix its tool
    # name rather than seeing a soft "didn't trigger."
    if find_tool(preferred["tool"]) is None:
        raise ValueError(f"unknown tool: {preferred['tool']!r}")

    # Run preferred. Tier mismatch (a soft input error) is reported as a
    # non-triggered result so the LLM can elevate and retry.
    pref_t0 = time.time()
    try:
        pref_result = _dispatch_inner_tool(
            preferred["tool"], preferred["arguments"], client)
        pref_block = {
            "tool": preferred["tool"],
            "result": pref_result,
            "triggered": True,
            "elapsed_ms": int((time.time() - pref_t0) * 1000),
        }
    except (ValueError, RuntimeError) as e:
        pref_block = {
            "tool": preferred["tool"],
            "result": None,
            "triggered": False,
            "reason": f"preferred tool raised: {type(e).__name__}: {e}",
            "elapsed_ms": int((time.time() - pref_t0) * 1000),
        }

    # Run variants (or log ruled-out ones).
    variant_blocks: list[dict] = []
    addressed_tools = {preferred["tool"]}
    for v in variants:
        if "tool" not in v:
            raise ValueError(
                "each variant must have a 'tool' field; got " + repr(v))
        addressed_tools.add(v["tool"])

        if "ruled_out_reason" in v:
            variant_blocks.append({
                "tool": v["tool"],
                "ruled_out": True,
                "reason": v["ruled_out_reason"],
            })
            continue

        if "arguments" not in v:
            raise ValueError(
                f"variant {v['tool']} must have 'arguments' (object) "
                "when not ruled out; got " + repr(v))

        var_t0 = time.time()
        try:
            var_result = _dispatch_inner_tool(
                v["tool"], v["arguments"], client)
            variant_blocks.append({
                "tool": v["tool"],
                "result": var_result,
                "triggered": True,
                "elapsed_ms": int((time.time() - var_t0) * 1000),
            })
        except (ValueError, RuntimeError) as e:
            variant_blocks.append({
                "tool": v["tool"],
                "triggered": False,
                "reason": f"{type(e).__name__}: {e}",
                "elapsed_ms": int((time.time() - var_t0) * 1000),
            })

    # Coverage gap: which family members weren't addressed?
    coverage_gap = []
    if has_family(preferred["tool"]):
        family_def = lookup(preferred["tool"])
        for member in family_def.family:
            if member not in addressed_tools:
                coverage_gap.append({
                    "tool": member,
                    "hint": family_def.alternatives.get(member, ""),
                })

    response = {
        "kind": "evaluate_with_variants_result",
        "preferred": pref_block,
        "variants": variant_blocks,
    }
    if coverage_gap:
        response["coverage_gap"] = {
            "missing": coverage_gap,
            "message": (
                "These family members were not addressed in either "
                "variants or ruled_out_reason. Either include them on "
                "your next call with proposed args, or document why "
                "ruled out via {tool: ..., arguments: ..., ruled_out_reason: ...}."
            ),
        }

    response["feedback_hint"] = (
        "If you'd like to provide reflective feedback on this call (was "
        "the preferred choice correct? would another variant have been "
        "better?), include it as a brief paragraph in your next message. "
        "The bridge logs your prose; no structured schema required."
    )

    return json.dumps(response, indent=2)


def _dispatch_inner_tool(name: str, arguments: dict, client: AgentClient) -> str:
    """Find a registered tool by name and invoke its handler. Used by
    evaluate_with_variants to run nested tool calls. Raises ValueError if
    the tool isn't registered."""
    tool = find_tool(name)
    if tool is None:
        raise ValueError(f"unknown tool: {name!r}")
    if not _can_satisfy_tier(tool.tier, client.current_tier):
        raise ValueError(
            f"tool {name!r} requires tier '{tool.tier}', current is "
            f"'{client.current_tier}'. Raise tier first via the appropriate "
            f"request_*_access tool (request_create_access / "
            f"request_update_access / request_delete_access / "
            f"request_extra_risky_access).")
    return tool.handler(arguments, client)


def _can_satisfy_tier(required: str, current: str) -> bool:
    """True if a connection at `current` tier can call a tool requiring
    `required` tier. Uses the CRUDX ladder defined in `_TIER_ORDER`."""
    if required == "always":
        return True
    return _TIER_ORDER.get(current, -1) >= _TIER_ORDER.get(required, 99)


# ---------------------------------------------------------------------------
# Tool table
#
# Order is significant only as documentation; the server filters by `tier` at
# `tools/list` time.

# Shared input schema for the per-tier `request_*_access` elevation tools.
_ELEVATION_INPUT_SCHEMA = {
    "type": "object",
    "properties": {"reason": {"type": "string", "minLength": 1}},
    "required": ["reason"],
    "additionalProperties": False,
}


TOOLS: list[ToolDef] = [
    # ----- Always available --------------------------------------------------
    ToolDef.from_spec(
        "system.info", "system.info", _h_agent_info,
        description=(
            "Get the agent's identity, current tier, advertised capabilities, "
            "and integrity level. Always call this first when you need to "
            "decide whether the agent can drive an elevated installer "
            "(check `integrity` and `uiaccess`)."
        ),
        tier="always",
    ),
    ToolDef(
        name="agent_reset",
        description=(
            "Recover a wedged bridge connection. Tears down the TCP "
            "connection to the agent and re-establishes hello + initialize. "
            "Use this when you see repeated `wire_error` responses on every "
            "tool call — that indicates the bridge's wire-framing parser has "
            "lost sync with the agent and cannot recover in-band. After "
            "reset, the connection's tier resets to `read`; you must call "
            "`request_*_access` again to re-elevate. Any active `watch.*` "
            "subscriptions are lost. Read-only verbs are auto-recovered by "
            "the bridge transparently — call `agent_reset` manually only "
            "when a non-idempotent verb (file.write, process.start, click) "
            "fails with `wire_error`, or when auto-recovery itself failed. "
            "Provide a one-sentence `reason` describing the failure pattern."
        ),
        tier="always",
        input_schema={
            "type": "object",
            "properties": {
                "reason": {
                    "type": "string",
                    "description": (
                        "One-sentence description of what failure pattern "
                        "prompted the reset (for audit-log clarity)."),
                },
            },
            "required": ["reason"],
        },
        handler=_h_agent_reset,
        # Side-effect: drops tier, kills subscriptions. Not destructive of
        # agent state, but not read-only either. Idempotent in the strong
        # sense: calling it twice in a row is equivalent to calling it once.
        idempotent_hint=True,
    ),
    ToolDef(
        name="request_create_access",
        description=(
            "Elevate the connection to the `create` tier so 'bring something "
            "new into existence' tools (directory.create, process.start) become "
            "callable. Provide a one-sentence `reason` so the audit log "
            "captures intent. After this returns OK, the tool list refreshes "
            "and additional tools become available."
        ),
        tier="always",
        input_schema=_ELEVATION_INPUT_SCHEMA,
        handler=_h_request_create_access,
    ),
    ToolDef(
        name="request_update_access",
        description=(
            "Elevate the connection to the `update` tier so 'modify existing' "
            "tools (synthetic input, file write, focus changes, click) become "
            "callable. Subsumes `create` per the ladder, so a single raise "
            "to update covers most non-destructive automation. Provide a "
            "one-sentence `reason`."
        ),
        tier="always",
        input_schema=_ELEVATION_INPUT_SCHEMA,
        handler=_h_request_update_access,
    ),
    ToolDef(
        name="request_delete_access",
        description=(
            "Elevate the connection to the `delete` tier so 'make existing "
            "things cease to be' tools (file.delete, process.kill, "
            "registry.delete) become callable. Subsumes update + create per "
            "the ladder. Use sparingly and only with a specific destructive "
            "operation in mind. Provide a one-sentence `reason`."
        ),
        tier="always",
        input_schema=_ELEVATION_INPUT_SCHEMA,
        handler=_h_request_delete_access,
    ),
    ToolDef(
        name="request_extra_risky_access",
        description=(
            "Elevate the connection to the top `extra_risky` tier so "
            "system-state-affecting tools (shutdown, reboot, logoff, "
            "hibernate, sleep, system.power.cancel) become callable. "
            "Subsumes everything below per the ladder. Highest privilege; "
            "only request with a specific power-state operation in mind. "
            "Provide a one-sentence `reason`."
        ),
        tier="always",
        input_schema=_ELEVATION_INPUT_SCHEMA,
        handler=_h_request_extra_risky_access,
    ),
    ToolDef(
        name="evaluate_with_variants",
        description=(
            "TEST MODE meta-tool. Use this in place of a direct tool call "
            "when you want the bridge to evaluate your verb choice against "
            "registered alternatives. Submit a `preferred` tool call plus "
            "`variants` you considered (each with full args, OR with a "
            "`ruled_out_reason` documenting why you decided against it). "
            "The bridge runs the preferred and the non-ruled-out variants, "
            "returns combined results, and adds a `coverage_gap` field "
            "naming any registry-known alternatives you didn't address. "
            "If a `coverage_gap` is reported, address the missing tools on "
            "your next call: either include them as variants with proposed "
            "args, or include them with `ruled_out_reason` documenting why. "
            "Outside test-mode benchmarks, prefer calling tools directly — "
            "this meta-tool exists for evaluating your tool choices, not "
            "for routine use."
        ),
        tier="always",
        input_schema={
            "type": "object",
            "properties": {
                "preferred": {
                    "type": "object",
                    "properties": {
                        "tool": {"type": "string"},
                        "arguments": {"type": "object"},
                    },
                    "required": ["tool", "arguments"],
                    "additionalProperties": False,
                },
                "variants": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "tool": {"type": "string"},
                            "arguments": {"type": "object"},
                            "ruled_out_reason": {"type": "string"},
                        },
                        "required": ["tool"],
                        "additionalProperties": False,
                    },
                    "default": [],
                },
            },
            "required": ["preferred"],
            "additionalProperties": False,
        },
        handler=_h_evaluate_with_variants,
    ),

    # ----- Read --------------------------------------------------------------
    ToolDef.from_spec(
        "screen.capture", "screen.capture", _h_take_screenshot,
        description=(
            "Capture the desktop, a region, a specific window, or a specific "
            "monitor. Returns base64 image bytes — expensive in LLM context. "
            "For TEXT ON SCREEN, prefer `vision.ocr` (returns plain strings). "
            "For WAITING ON VISUAL CHANGE (progress bars, page loads), prefer "
            "`wait_for_visual_change` (watch.region --until-change) which "
            "blocks server-side. Reach for the raw screenshot only when you "
            "need the pixels themselves (e.g. visual confirmation of a "
            "selection). Provide at most one of `region` / `window` / "
            "`monitor`."
        ),
    ),
    ToolDef.from_spec(
        "window.list", "window.list", _h_list_windows,
        description=(
            "List top-level windows. By default returns only visible windows; "
            "pass `visible_only=false` to include hidden ones. Optionally "
            "filter by `pid`."
        ),
    ),
    ToolDef.from_spec(
        "window.find", "window.find", _h_find_window,
        description=(
            "Find the first top-level window whose title matches `pattern`. "
            "Default `match` is `substring` (case-insensitive); pass `match` "
            "= `prefix`/`exact`/`glob`/`regex` for stricter semantics. "
            "Returns the window's handle / pid / title / bounds, or "
            "`not_found`."
        ),
    ),
    ToolDef.from_spec(
        "element.list", "element.list", _h_list_elements,
        description=(
            "Enumerate visible UI Automation elements in a region or "
            "across the desktop. Cheaper than `screen.capture` when you "
            "want to know what UI controls exist. Restrict to a `region` "
            "(`{x,y,w,h}` object) to keep the response manageable."
        ),
    ),
    ToolDef.from_spec(
        "element.find", "element.find", _h_find_element,
        description=(
            "Single-shot search for a UI Automation element by role / name "
            "/ automation_id, optionally rooted at a window handle. "
            "IF YOU'RE POLLING because the element may not exist yet, use "
            "`element.wait` instead — it polls server-side every 250 ms "
            "with one round-trip and supports `flags_required:[\"enabled\"]` "
            "to wait until the element is actually interactable. Returns "
            "the element's accessible properties and bounds. ERR "
            "`uia_blind` means the element may exist but UIA can't see "
            "across an integrity barrier — see PROTOCOL.md §8."
        ),
    ),
    ToolDef.from_spec(
        "element.wait", "element.wait", _h_wait_for_element,
        description=(
            "Block until a UIA element matching `role` / `name` / "
            "`automation_id` appears, or the timeout expires. Replaces "
            "screenshot-poll loops for waiting on a wizard's next button "
            "to render."
        ),
    ),
    # R8 — element.range_value (agent-ahead-of-spec; not in protocol/spec/ yet,
    # so manual ToolDef rather than from_spec). See protocol#95.
    ToolDef(
        name="get_range_value",
        description=(
            "Read UIA RangeValuePattern numeric state from a progress bar, "
            "slider, or scrollbar. Returns `{min, max, value, "
            "small_change?, large_change?, readonly?}`. Surfaces ERR "
            "`not_supported_by_target` when the element doesn't implement "
            "the pattern."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "handle": {
                    "type": "string",
                    "description": (
                        "Element handle (`elt:N`) from find_element / "
                        "list_elements."
                    ),
                },
            },
            "required": ["handle"],
            "additionalProperties": False,
        },
        handler=_h_get_range_value,
        read_only_hint=True,
        wire_verb="element.range_value",
    ),
    # R9 — element.get_text (agent-ahead-of-spec).
    ToolDef(
        name="get_element_text",
        description=(
            "Read the element's text content via UIA TextPattern (with "
            "ValuePattern and Name fallbacks). USE THIS OVER `take_screenshot`"
            " + Read for reading on-page text — returns plain strings instead "
            "of base64 image bytes; supports pagination via `offset` / "
            "`max_length`. The total underlying length is in the response so "
            "you can decide whether to paginate or narrow the handle to a "
            "sub-element when the document is large."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "handle": {
                    "type": "string",
                    "description": "Element handle in `elt:N` form.",
                },
                "max_length": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 262144,
                    "default": 16384,
                    "description": (
                        "Maximum bytes of text to return. Default 16 KiB; "
                        "cap 256 KiB."
                    ),
                },
                "offset": {
                    "type": "integer",
                    "minimum": 0,
                    "default": 0,
                    "description": (
                        "Byte offset to start reading from. Pair with "
                        "max_length for paginated reads."
                    ),
                },
            },
            "required": ["handle"],
            "additionalProperties": False,
        },
        handler=_h_get_element_text,
        read_only_hint=True,
        wire_verb="element.get_text",
    ),
    # R10 — element.search (agent-ahead-of-spec). The killer feature for
    # finding known phrases in long documents like Wikipedia articles.
    ToolDef(
        name="search_elements",
        description=(
            "Server-side multi-pattern text search over a root element's "
            "TextPattern document range. Returns excerpts (NOT full text) — "
            "USE THIS OVER `take_screenshot` + OCR or `get_element_text` + "
            "client-side string search when looking for known phrases in long "
            "documents (e.g. Wikipedia articles). Each hit always carries "
            "`handle` + `enclosing_role`; set `include_bounds:true` to get "
            "screen rectangles for drag-select workflows (drag from the "
            "first rect's start to the last rect's end). `patterns_unmatched` "
            "is the quick-no signal — if your pattern doesn't appear, you "
            "learn that in one round-trip instead of after a full read."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "root": {
                    "type": "string",
                    "description": (
                        "UIA element handle (`elt:N`) to scope the search. "
                        "Window handles (`win:0xHEX`) from list_windows are "
                        "NOT accepted — you must first call find_element with "
                        "`root: <win-handle>` and a role like `document` or "
                        "`pane` to get an `elt:N`, then pass that handle here. "
                        "Typical chain: list_windows → find_element "
                        "(role:'document', root:win-handle) → search_elements "
                        "(root: that elt:N)."
                    ),
                },
                "patterns": {
                    "type": "array",
                    "items": {"type": "string", "minLength": 1},
                    "minItems": 1,
                    "maxItems": 16,
                    "description": "1..16 phrases to search for.",
                },
                "match": {
                    "type": "string",
                    "enum": ["substring", "regex"],
                    "default": "substring",
                },
                "case_sensitive": {"type": "boolean", "default": False},
                "context_chars": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 4096,
                    "default": 80,
                    "description": (
                        "Characters of context on each side of each match."
                    ),
                },
                "max_hits_per_pattern": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 100,
                    "default": 5,
                },
                "include_bounds": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "When true, each hit carries an array of screen "
                        "rectangles (one per visual line — wrapped matches "
                        "produce multiple rects). For drag-select workflows."
                    ),
                },
            },
            "required": ["root", "patterns"],
            "additionalProperties": False,
        },
        handler=_h_search_elements,
        read_only_hint=True,
        wire_verb="element.search",
    ),
    ToolDef.from_spec(
        "file.wait", "file.wait", _h_wait_for_file,
        description=(
            "Block until a file matching `glob` appears on the filesystem, "
            "or `timeout_ms` expires. The most reliable signal for "
            "`did the download / installer output land?` flows. Wildcards: "
            "`*` (single path component), `?` (single char)."
        ),
    ),
    ToolDef.from_spec(
        "file.read", "file.read", _h_read_file,
        description=(
            "Read a UTF-8 text file. Binary content is returned base64-encoded "
            "with a clear marker. Optional `offset` / `length` for partial "
            "reads."
        ),
    ),
    ToolDef.from_spec(
        "directory.list", "directory.list", _h_list_directory,
        description=(
            "List the entries in a directory. Returns name / type / size / "
            "mtime per entry. Pass `recursive: true` for a subtree walk; "
            "`pattern` to filter by name; `limit` to cap result count."
        ),
    ),
    ToolDef.from_spec(
        "process.list", "process.list", _h_list_processes,
        description=(
            "Snapshot of running processes. IF YOU'RE WAITING ON A PROCESS "
            "to start or exit, use `wait_for_process_exit` (process.wait) "
            "instead — it blocks server-side. Optionally filter by image-"
            "name substring (`pattern`); pass `include_counters: true` to "
            "add CPU/memory/handle counts. Session-0 service processes "
            "excluded unless `include_system: true`."
        ),
    ),
    ToolDef.from_spec(
        "clipboard.get", "clipboard.get", _h_get_clipboard,
        description="Read the clipboard's text content. Empty if nothing is on the clipboard.",
    ),
    ToolDef(
        name="wait_for_visual_change",
        description=(
            "Block until a region of the screen changes, OR until timeout. "
            "Wraps `watch.region --until-change`. Returns the captured PNG "
            "(base64-encoded) of the post-change frame. Use for 'wait until "
            "the dialog/page/UI updates' workflows. For animated targets, "
            "the watch fires on any pixel change including animation noise — "
            "future `--normalized` flag will dampen this. For UIA-visible "
            "elements, prefer `wait_for_element` (cheaper, semantically "
            "richer)."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "region": {
                    "type": "string",
                    "description": "x,y,w,h in screen coordinates",
                    "pattern": r"^\d+,\d+,\d+,\d+$",
                },
                "interval_ms": {
                    "type": "integer", "minimum": 50, "default": 500,
                    "description": "polling interval; lower = faster reaction, more CPU",
                },
                "timeout_ms": {
                    "type": "integer", "minimum": 100, "default": 30000,
                    "description": "max wait before returning a timeout",
                },
            },
            "required": ["region"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_visual_change,
        read_only_hint=True,
    ),
    ToolDef(
        name="wait_for_window",
        description=(
            "Block until a top-level Win32 window with a matching title "
            "prefix appears or disappears, OR until timeout. Wraps "
            "`watch.window`. Returns the first window_appeared / window_gone "
            "event. Useful for waiting on installer dialogs, error popups, "
            "or any separate top-level window. Won't fire for Unity / web / "
            "canvas overlays — those aren't Win32 windows; use "
            "`wait_for_visual_change` or `wait_for_element` instead."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "title_prefix": {
                    "type": "string", "minLength": 1,
                    "description": "case-insensitive prefix to match against window titles",
                },
                "timeout_ms": {
                    "type": "integer", "minimum": 100, "default": 30000,
                },
            },
            "required": ["title_prefix"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_window,
        read_only_hint=True,
    ),
    ToolDef(
        name="wait_for_process_exit",
        description=(
            "Block until a specific PID exits, OR until timeout. Wraps "
            "`watch.process` (auto-cancels on exit). Returns the exit code "
            "via the process_exit event. Critical for installer / build / "
            "long-running-command workflows: `launch` returns the PID, then "
            "`wait_for_process_exit` lets you proceed when it's done."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "pid": {"type": "integer", "minimum": 0},
                "timeout_ms": {
                    "type": "integer", "minimum": 100, "default": 60000,
                },
            },
            "required": ["pid"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_process_exit,
        read_only_hint=True,
    ),
    ToolDef(
        name="wait_for_file_change",
        description=(
            "Block until any filesystem change matches the given path or "
            "glob pattern, OR until timeout. Wraps `watch.file`. Returns "
            "the first change event (created / deleted / modified / "
            "renamed). Different from `wait_for_file` which waits for a "
            "path to *exist*; this fires on any change including delete "
            "or modification of an existing file."
        ),
        tier="read",
        input_schema={
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string", "minLength": 1,
                    "description": "absolute path or path-with-glob (e.g. C:\\\\foo\\\\*.log)",
                },
                "timeout_ms": {
                    "type": "integer", "minimum": 100, "default": 60000,
                },
            },
            "required": ["pattern"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_file_change,
        read_only_hint=True,
    ),

    # ----- Update ------------------------------------------------------------
    ToolDef.from_spec(
        "window.focus", "window.focus", _h_focus_window,
        description=(
            "Bring a window to the foreground. Returns the previously-"
            "foreground window's handle (so the caller can restore it "
            "later if desired). ERR `lock_held` means Windows refused — "
            "usually because no foreground process granted us permission. "
            "See `docs/windows-automation-notes.md`."
        ),
    ),
    ToolDef.from_spec(
        "window.close", "window.close", _h_close_window,
        description=(
            "Send WM_CLOSE to a window (graceful — the app may decline, "
            "e.g. unsaved-document prompt)."
        ),
    ),
    # Composite: dispatches to one of element.invoke / element.find_invoke /
    # element.at_invoke depending on which fields are populated. Kept under a
    # semantic name (with the `element.` prefix) because no single wire verb
    # captures the whole tool.
    ToolDef(
        name="element.click",
        description=(
            "Click a UIA element. Provide ONE of: `handle` from a prior "
            "`element.find` / `element.list` (legacy `element_id` accepted); "
            "`role`+`name` to find and click in one round-trip (dispatches "
            "to the wire's `element.find_invoke`); or `x`+`y` to invoke the "
            "element at a screen coordinate (dispatches to `element.at_"
            "invoke`). ERR `uipi_blocked` means the target window is at "
            "higher integrity than the agent — see `request_update_access` "
            "/ PROTOCOL.md §8."
        ),
        tier="update",
        input_schema={
            "type": "object",
            "properties": {
                "handle": {"type": "string", "description": "elt:N from a prior find/list"},
                "role": {"type": "string"},
                "name": {"type": "string"},
                "automation_id": {"type": "string"},
                "root": {"type": "string"},
                "timeout_ms": {"type": "integer", "minimum": 0},
                "x": {"type": "integer"},
                "y": {"type": "integer"},
            },
            "additionalProperties": False,
        },
        handler=_h_click_element,
    ),
    ToolDef.from_spec(
        "element.set_text", "element.set_text", _h_set_element_text,
        description=(
            "Replace the text in a UIA edit / text element via "
            "ValuePattern. Pair with `element.find` to locate the field "
            "first."
        ),
    ),
    ToolDef.from_spec(
        "input.keyboard.type", "input.keyboard.type", _h_type_text,
        description=(
            "Type literal Unicode text into the focused window via "
            "SendInput. Handles characters that would be hazardous to "
            "send as keystrokes. Note: kernel-level RawInput / DirectInput "
            "targets (Unity, most game frameworks) won't observe synthetic "
            "input — see spec/operators/05-footguns."
        ),
    ),
    ToolDef.from_spec(
        "input.keyboard.key", "input.keyboard.key", _h_press_keys,
        description=(
            "Press a named key (`enter`, `tab`, `F4`, `a`, …) with optional "
            "`modifiers` (array of `ctrl`/`shift`/`alt`/`meta`). Atomic "
            "press-release; pass `duration_ms` to control the press-to-"
            "release gap (capped at 1000 ms)."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.click", "input.mouse.click", _h_click_at,
        description=(
            "Click at an absolute screen coordinate. Prefer `element.click` "
            "for UI buttons — coordinates drift across DPI / window-"
            "position changes. Optional `double`/`triple` for OS-recognised "
            "multi-click; `modifiers` is an array of `ctrl`/`shift`/`alt`/"
            "`meta`."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.move", "input.mouse.move", _h_move_mouse,
        description=(
            "Move the mouse cursor without clicking. Pass `relative:true` "
            "to treat (x, y) as deltas from the current cursor position. "
            "Useful as a precursor to `input.mouse.drag` to set the drag "
            "start point."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.press", "input.mouse.press", _h_press_mouse,
        description=(
            "Press (and HOLD) a mouse button at (x, y). Must be paired "
            "with `input.mouse.release` — the OS keeps the button down "
            "until the matching release. For atomic drags, prefer "
            "`input.mouse.drag` which packages press+move+release in one "
            "SendInput batch."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.release", "input.mouse.release", _h_release_mouse,
        description=(
            "Release the mouse button at (x, y). Pair with a prior "
            "`input.mouse.press`."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.drag", "input.mouse.drag", _h_drag_mouse,
        description=(
            "ATOMIC press-move-release at the current cursor position. "
            "Holds `button` (default left), smoothly moves to (x, y) over "
            "`steps` (default 10) interpolated positions, then releases. "
            "Single SendInput batch — the OS sees continuous motion "
            "(passes the drag-threshold check `SM_CXDRAG`). For "
            "drag-from-A-to-B: first call `input.mouse.move` with A's "
            "coords, then `input.mouse.drag` with B's coords. THIS IS THE "
            "RIGHT TOOL FOR DRAG-SELECTING TEXT (Wikipedia quotes), "
            "dragging sliders, drawing selection rectangles, etc."
        ),
    ),
    ToolDef.from_spec(
        "input.mouse.scroll", "input.mouse.scroll", _h_scroll_mouse,
        description=(
            "Scroll the mouse wheel at (x, y). Positive `delta_y` scrolls "
            "down, negative up. Family-dependent extras (horizontal "
            "scroll, tilt-wheel) per the spec."
        ),
    ),
    ToolDef(
        name="file.write",
        description=(
            "Write UTF-8 text to a file on the target (overwrites). For "
            "binary content the LLM has in hand, use `file.write_b64`. For "
            "binary content from a path on the controller (zip, exe, DLLs, "
            "archives), use `file.upload` — bytes don't pass through MCP."
        ),
        tier="update",
        input_schema={
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "content": {"type": "string"},
            },
            "required": ["path", "content"],
            "additionalProperties": False,
        },
        handler=_h_write_file,
    ),
    ToolDef(
        name="file.write_b64",
        description=(
            "Write base64-encoded binary content to a file on the target "
            "(overwrites). Use when the LLM has the bytes in hand as a "
            "base64 string — typically generated content or small fixtures. "
            "For binary content from a path on the controller, prefer "
            "`file.upload` so the bytes never pass through the LLM context."
        ),
        tier="update",
        input_schema={
            "type": "object",
            "properties": {
                "path":        {"type": "string"},
                "content_b64": {"type": "string",
                                 "description": "base64-encoded file bytes"},
            },
            "required": ["path", "content_b64"],
            "additionalProperties": False,
        },
        handler=_h_write_file_b64,
    ),
    ToolDef(
        name="file.upload",
        description=(
            "Read a file from the controller (where this MCP bridge runs) "
            "and write it to a path on the target (where the agent runs). "
            "Bytes flow filesystem-to-filesystem without passing through "
            "the LLM transport, so this is the right tool for binary "
            "distribution (zip, exe, DLLs, archives, game assets, images) "
            "and for any file size — files larger than 16 MB are chunked "
            "internally over the wire via file.write_at. Fault-tolerant: "
            "retries each chunk on transport failure with exponential "
            "backoff, then halves the chunk size and retries from the same "
            "offset if a size keeps timing out. Bottoms out at 256 KB "
            "before reporting failure with progress detail. For text "
            "content already in the LLM's context, use `file.write`."
        ),
        tier="update",
        input_schema={
            "type": "object",
            "properties": {
                "source_path": {
                    "type": "string",
                    "description": ("Absolute path on the controller "
                                    "(where the bridge runs) to read from."),
                },
                "destination_path": {
                    "type": "string",
                    "description": ("Absolute path on the target "
                                    "(where the agent runs) to write to."),
                },
            },
            "required": ["source_path", "destination_path"],
            "additionalProperties": False,
        },
        handler=_h_upload_file,
    ),
    ToolDef.from_spec(
        "process.start", "process.start", _h_launch,
        description=(
            "Spawn a process via CreateProcess. Returns the spawned PID. "
            "Pass `argv` as an array (argv[0] is the executable path or "
            "name; argv[1..] are arguments). For shell-verb / spaces / "
            "unicode cases, prefer `process.shell`. Optionally provide "
            "`stdin` (UTF-8 text) and `cwd`."
        ),
    ),
    ToolDef.from_spec(
        "process.shell", "process.shell", _h_shell_open,
        description=(
            "ShellExecuteEx — opens a path / URL with the default-"
            "associated app. Use for `start http://...` or `start file.pdf` "
            "semantics. Pass `verb='runas'` to elevate (triggers UAC). "
            "Returns spawned PID (may be 0 for verbs that don't spawn a "
            "new process)."
        ),
    ),

    # ----- Delete ------------------------------------------------------------
    ToolDef.from_spec(
        "process.kill", "process.kill", _h_kill_process,
        description=(
            "TerminateProcess — abrupt, no save. For graceful shutdown "
            "try `window.close` first."
        ),
    ),
    ToolDef.from_spec(
        "file.delete", "file.delete", _h_delete_file,
        description=(
            "Delete a file or empty directory. Hard delete — bypasses Recycle "
            "Bin / Trash. For non-empty directories use `directory.remove` with "
            "`recursive=true`."
        ),
    ),

    # ----- Extra risky -------------------------------------------------------
    # system.power.cancel's spec is CRUDX `U` (the act of cancelling a
    # pending shutdown is conceptually an update). The bridge gates it
    # behind extra_risky anyway: a power-cancel call only makes sense
    # alongside a pending shutdown (extra_risky), and surfacing it at
    # update tier would leak the existence of pending power state to
    # any update-tier caller.
    ToolDef.from_spec(
        "system.power.cancel", "system.power.cancel",
        _h_cancel_pending_shutdown,
        tier="extra_risky",
        description=(
            "Abort a pending delayed shutdown / reboot / logoff scheduled "
            "via the `system.power.*` family. Returns ERR not_found if no "
            "shutdown is pending. Tier `extra_risky` matches the action "
            "being cancelled."
        ),
    ),
]


# ---------------------------------------------------------------------------
# Lookup helpers

def tools_by_tier(current_tier: str) -> list[ToolDef]:
    """Return tools the LLM should see at `current_tier`. `always` tools are
    always included; tiered tools are included if `current_tier` reaches them
    on the CRUDX ladder (read < create < update < delete < extra_risky)."""
    cur = _TIER_ORDER.get(current_tier, _TIER_ORDER["read"])
    out: list[ToolDef] = []
    for t in TOOLS:
        if t.tier == "always" or _TIER_ORDER.get(t.tier, 99) <= cur:
            out.append(t)
    return out


def find_tool(name: str) -> Optional[ToolDef]:
    for t in TOOLS:
        if t.name == name:
            return t
    return None
