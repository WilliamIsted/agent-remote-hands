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

Tools are tagged with the wire-protocol tier they require. The server consults
that tag when answering `tools/list` so an `observe`-tier session sees only
the read tools; the LLM raises tier explicitly via `request_drive_access`
or `request_power_access`, which the server treats as an elevation request
and (on success) emits a `tools/list_changed` notification so the client
re-queries and sees the broader surface.

Tool naming: semantic over wire-mechanical (e.g. `click_element`, not
`element_invoke_at_xy`) per the agent CLAUDE.md guidance — the LLM reads
the tool description and decides intent from it.

This is a starter set covering the most common automation flows. Add tools
here as new agent verbs ship; the dispatch table at the bottom is the only
place that changes.
"""

from __future__ import annotations

import base64
import json
import os
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from agent_client import AgentClient, ErrResponse, OkResponse, read_token_file


# ---------------------------------------------------------------------------
# Tool registry types

@dataclass
class ToolDef:
    name: str
    description: str
    tier: str  # "observe" | "drive" | "power" | "always"
    input_schema: dict
    handler: Callable[[dict, AgentClient], str]
    # MCP standard annotations — the client uses these for safety prompting.
    read_only_hint: bool = False
    destructive_hint: bool = False
    idempotent_hint: bool = False
    open_world_hint: bool = False
    annotations: dict = field(default_factory=dict)

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


def _h_request_drive_access(args: dict, client: AgentClient) -> str:
    reason = args.get("reason", "").strip()
    if not reason:
        raise ValueError("reason is required — explain why drive tier is needed")

    if client.can_satisfy("drive"):
        return f"Already at {client.current_tier} tier; drive-tier tools are available."

    token = client._token or read_token_file(os.environ.get("REMOTE_HANDS_TOKEN_PATH"))
    if token is None:
        raise RuntimeError(
            "no token available — set REMOTE_HANDS_TOKEN_PATH to point at the "
            "agent's token file (default %ProgramData%\\AgentRemoteHands\\token)")
    client.set_token(token)

    r = client.tier_raise("drive")
    if isinstance(r, ErrResponse):
        raise RuntimeError(f"tier_raise drive failed: {r.code} {json.dumps(r.detail)}")
    return f"Elevated to drive tier (reason: {reason}). Drive-tier tools will appear on the next tools/list query."


def _h_request_power_access(args: dict, client: AgentClient) -> str:
    reason = args.get("reason", "").strip()
    if not reason:
        raise ValueError("reason is required — explain why power tier is needed (destructive operations)")

    if client.can_satisfy("power"):
        return f"Already at {client.current_tier} tier; power-tier tools are available."

    token = client._token or read_token_file(os.environ.get("REMOTE_HANDS_TOKEN_PATH"))
    if token is None:
        raise RuntimeError("no token available — see request_drive_access for setup")
    client.set_token(token)

    # Power requires drive first.
    if not client.can_satisfy("drive"):
        r = client.tier_raise("drive")
        if isinstance(r, ErrResponse):
            raise RuntimeError(f"tier_raise drive failed: {r.code} {json.dumps(r.detail)}")

    r = client.tier_raise("power")
    if isinstance(r, ErrResponse):
        raise RuntimeError(f"tier_raise power failed: {r.code} {json.dumps(r.detail)}")
    return f"Elevated to power tier (reason: {reason}). Power-tier tools will appear on the next tools/list query."


# --- Observe -----------------------------------------------------------------

def _h_take_screenshot(args: dict, client: AgentClient) -> str:
    fmt = args.get("format", "png")
    region = args.get("region")
    window = args.get("window")
    call_args = ["--format", fmt]
    if region:
        call_args += ["--region", region]
    if window:
        call_args += ["--window", window]
    r = _ok_or_raise(client.request("screen.capture", *call_args), "screen.capture")
    # Don't dump the bytes inline — return a compact marker the LLM understands.
    return (f"Captured {len(r.payload)} bytes ({fmt}). To view, save the payload "
            f"to a file or pipe through an image-viewer. Use --region or --window "
            f"to crop and shrink the response.")


def _h_list_windows(args: dict, client: AgentClient) -> str:
    extra = []
    if args.get("title_prefix"):
        extra += ["--filter", args["title_prefix"]]
    if args.get("include_all"):
        extra += ["--all"]
    return _format_ok(_ok_or_raise(client.request("window.list", *extra), "window.list"))


def _h_find_window(args: dict, client: AgentClient) -> str:
    pattern = args["title_pattern"]
    return _format_ok(_ok_or_raise(client.request("window.find", pattern), "window.find"))


def _h_list_elements(args: dict, client: AgentClient) -> str:
    extra = []
    if args.get("region"):
        extra += ["--region", args["region"]]
    return _format_ok(_ok_or_raise(client.request("element.list", *extra), "element.list"))


def _h_find_element(args: dict, client: AgentClient) -> str:
    role = args["role"]
    name_pattern = args["name_pattern"]
    return _format_ok(_ok_or_raise(client.request("element.find", role, name_pattern), "element.find"))


def _h_wait_for_element(args: dict, client: AgentClient) -> str:
    role = args["role"]
    name_pattern = args["name_pattern"]
    timeout_ms = str(int(args.get("timeout_ms", 30000)))
    return _format_ok(_ok_or_raise(
        client.request("element.wait", role, name_pattern, timeout_ms), "element.wait"))


def _h_wait_for_file(args: dict, client: AgentClient) -> str:
    pattern = args["pattern"]
    timeout_ms = str(int(args.get("timeout_ms", 60000)))
    return _format_ok(_ok_or_raise(
        client.request("file.wait", pattern, timeout_ms), "file.wait"))


def _h_read_file(args: dict, client: AgentClient) -> str:
    path = args["path"]
    r = _ok_or_raise(client.request("file.read", path), "file.read")
    try:
        return r.payload.decode("utf-8")
    except UnicodeDecodeError:
        return f"<binary {len(r.payload)} bytes, base64>\n" + base64.b64encode(r.payload).decode("ascii")


def _h_list_directory(args: dict, client: AgentClient) -> str:
    path = args["path"]
    return _format_ok(_ok_or_raise(client.request("file.list", path), "file.list"))


def _h_list_processes(args: dict, client: AgentClient) -> str:
    extra = []
    if args.get("name_filter"):
        extra += ["--filter", args["name_filter"]]
    return _format_ok(_ok_or_raise(client.request("process.list", *extra), "process.list"))


def _h_get_clipboard(args: dict, client: AgentClient) -> str:
    r = _ok_or_raise(client.request("clipboard.read"), "clipboard.read")
    try:
        return r.payload.decode("utf-8")
    except UnicodeDecodeError:
        return f"<{len(r.payload)} non-text bytes>"


# --- Drive -------------------------------------------------------------------

def _h_focus_window(args: dict, client: AgentClient) -> str:
    return _format_ok(_ok_or_raise(client.request("window.focus", args["hwnd"]), "window.focus"))


def _h_close_window(args: dict, client: AgentClient) -> str:
    return _format_ok(_ok_or_raise(client.request("window.close", args["hwnd"]), "window.close"))


def _h_click_element(args: dict, client: AgentClient) -> str:
    """Click a UIA element. Three call shapes for the LLM to pick:
       - element_id (e.g. \"elt:7\" from a prior find/list)
       - role + name_pattern (compound find_invoke)
       - x + y (compound at_invoke)
    """
    elt_id = args.get("element_id")
    role = args.get("role")
    name_pattern = args.get("name_pattern")
    x = args.get("x")
    y = args.get("y")

    if elt_id:
        return _format_ok(_ok_or_raise(client.request("element.invoke", elt_id), "element.invoke"))
    if role and name_pattern:
        return _format_ok(_ok_or_raise(
            client.request("element.find_invoke", role, name_pattern), "element.find_invoke"))
    if x is not None and y is not None:
        return _format_ok(_ok_or_raise(
            client.request("element.at_invoke", str(int(x)), str(int(y))), "element.at_invoke"))
    raise ValueError("click_element requires one of: element_id, role+name_pattern, or x+y")


def _h_set_element_text(args: dict, client: AgentClient) -> str:
    elt_id = args["element_id"]
    text = args["text"].encode("utf-8")
    return _format_ok(_ok_or_raise(
        client.request("element.set_text", elt_id, str(len(text)), payload=text),
        "element.set_text"))


def _h_type_text(args: dict, client: AgentClient) -> str:
    text = args["text"].encode("utf-8")
    return _format_ok(_ok_or_raise(
        client.request("input.type", str(len(text)), payload=text), "input.type"))


def _h_press_keys(args: dict, client: AgentClient) -> str:
    key = args["key"]
    extra = []
    if args.get("modifiers"):
        extra += ["--modifiers", args["modifiers"]]
    return _format_ok(_ok_or_raise(client.request("input.key", key, *extra), "input.key"))


def _h_click_at(args: dict, client: AgentClient) -> str:
    x = str(int(args["x"]))
    y = str(int(args["y"]))
    extra = []
    if args.get("button"):
        extra += ["--button", args["button"]]
    return _format_ok(_ok_or_raise(client.request("input.click", x, y, *extra), "input.click"))


def _h_write_file(args: dict, client: AgentClient) -> str:
    path = args["path"]
    payload = args["content"].encode("utf-8")
    return _format_ok(_ok_or_raise(
        client.request("file.write", path, str(len(payload)), payload=payload),
        "file.write"))


def _h_launch(args: dict, client: AgentClient) -> str:
    command_line = args["command_line"]
    return _format_ok(_ok_or_raise(client.request("process.start", command_line), "process.start"))


def _h_shell_open(args: dict, client: AgentClient) -> str:
    path = args["path"]
    extra = []
    if args.get("args"):
        extra += ["--args", args["args"]]
    if args.get("verb"):
        extra += ["--verb", args["verb"]]
    return _format_ok(_ok_or_raise(client.request("process.shell", path, *extra), "process.shell"))


# --- Power -------------------------------------------------------------------

def _h_kill_process(args: dict, client: AgentClient) -> str:
    pid = str(int(args["pid"]))
    return _format_ok(_ok_or_raise(client.request("process.kill", pid), "process.kill"))


def _h_delete_file(args: dict, client: AgentClient) -> str:
    return _format_ok(_ok_or_raise(client.request("file.delete", args["path"]), "file.delete"))


def _h_cancel_pending_shutdown(args: dict, client: AgentClient) -> str:
    return _format_ok(_ok_or_raise(client.request("system.power.cancel"), "system.power.cancel"))


# ---------------------------------------------------------------------------
# Tool table
#
# Order is significant only as documentation; the server filters by `tier` at
# `tools/list` time.

TOOLS: list[ToolDef] = [
    # ----- Always available --------------------------------------------------
    ToolDef(
        name="agent_info",
        description=(
            "Get the agent's identity, current tier, advertised capabilities, "
            "and integrity level. Always call this first when you need to "
            "decide whether the agent can drive an elevated installer "
            "(check `integrity` and `uiaccess`)."
        ),
        tier="always",
        input_schema={"type": "object", "properties": {}, "additionalProperties": False},
        handler=_h_agent_info,
        read_only_hint=True,
        idempotent_hint=True,
    ),
    ToolDef(
        name="request_drive_access",
        description=(
            "Elevate the connection to the `drive` tier so write / input / "
            "process-launch tools become callable. Provide a one-sentence "
            "`reason` so the audit log captures intent. After this returns "
            "OK, the tool list refreshes and additional tools become "
            "available; call `agent_info` if you want to confirm."
        ),
        tier="always",
        input_schema={
            "type": "object",
            "properties": {"reason": {"type": "string", "minLength": 1}},
            "required": ["reason"],
            "additionalProperties": False,
        },
        handler=_h_request_drive_access,
    ),
    ToolDef(
        name="request_power_access",
        description=(
            "Elevate the connection to the `power` tier so destructive tools "
            "(file delete, process kill, shutdown) become callable. Implies "
            "drive tier as a stepping stone. Use this sparingly and only "
            "with a specific destructive operation in mind."
        ),
        tier="always",
        input_schema={
            "type": "object",
            "properties": {"reason": {"type": "string", "minLength": 1}},
            "required": ["reason"],
            "additionalProperties": False,
        },
        handler=_h_request_power_access,
    ),

    # ----- Observe -----------------------------------------------------------
    ToolDef(
        name="take_screenshot",
        description=(
            "Capture the desktop, a region, or a specific window. Returns the "
            "byte count and format (the bytes themselves are too large for "
            "inline viewing — save to file or use a smaller region)."
        ),
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {
                "format": {"type": "string", "enum": ["png", "webp", "bmp"], "default": "png"},
                "region": {"type": "string", "description": "x,y,w,h"},
                "window": {"type": "string", "description": "win:0xHEX hwnd"},
            },
            "additionalProperties": False,
        },
        handler=_h_take_screenshot,
        read_only_hint=True,
    ),
    ToolDef(
        name="list_windows",
        description="List visible top-level windows. Phantom (off-screen, zero-area, NOACTIVATE) windows are filtered by default; pass `include_all=true` to keep them.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {
                "title_prefix": {"type": "string"},
                "include_all": {"type": "boolean", "default": False},
            },
            "additionalProperties": False,
        },
        handler=_h_list_windows,
        read_only_hint=True,
        idempotent_hint=True,
    ),
    ToolDef(
        name="find_window",
        description="Find the first visible window whose title starts with the given pattern (case-insensitive). Returns the window's hwnd / pid / bounds, or `not_found`.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {"title_pattern": {"type": "string", "minLength": 1}},
            "required": ["title_pattern"],
            "additionalProperties": False,
        },
        handler=_h_find_window,
        read_only_hint=True,
    ),
    ToolDef(
        name="list_elements",
        description="Enumerate visible UI Automation elements. Restrict to a region to keep the response manageable.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {"region": {"type": "string", "description": "x,y,w,h"}},
            "additionalProperties": False,
        },
        handler=_h_list_elements,
        read_only_hint=True,
    ),
    ToolDef(
        name="find_element",
        description=(
            "Find a UI Automation element by role + name substring. Returns "
            "the element id (`elt:N`) which is valid for `click_element` and "
            "`set_element_text` on the same MCP session. ERR `uia_blind` "
            "means the element may exist but UIA can't see across an "
            "integrity barrier — see PROTOCOL.md §8."
        ),
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {
                "role": {"type": "string", "description": "e.g. button, edit, listitem"},
                "name_pattern": {"type": "string", "description": "case-insensitive substring of the Name property"},
            },
            "required": ["role", "name_pattern"],
            "additionalProperties": False,
        },
        handler=_h_find_element,
        read_only_hint=True,
    ),
    ToolDef(
        name="wait_for_element",
        description=(
            "Block until a UIA element matching role + name appears, or the "
            "timeout expires. Replaces screenshot-poll loops for waiting on "
            "a wizard's next button to render."
        ),
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {
                "role": {"type": "string"},
                "name_pattern": {"type": "string"},
                "timeout_ms": {"type": "integer", "minimum": 0, "default": 30000},
            },
            "required": ["role", "name_pattern"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_element,
        read_only_hint=True,
    ),
    ToolDef(
        name="wait_for_file",
        description="Block until a file matching the path or glob appears, or timeout. Most reliable signal for `did the download / installer output land?` flows.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {
                "pattern": {"type": "string", "description": "absolute path or glob (e.g. C:\\\\Users\\\\x\\\\Downloads\\\\Firefox*.exe)"},
                "timeout_ms": {"type": "integer", "minimum": 0, "default": 60000},
            },
            "required": ["pattern"],
            "additionalProperties": False,
        },
        handler=_h_wait_for_file,
        read_only_hint=True,
    ),
    ToolDef(
        name="read_file",
        description="Read a UTF-8 text file. Binary content is returned base64-encoded with a clear marker.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
            "additionalProperties": False,
        },
        handler=_h_read_file,
        read_only_hint=True,
    ),
    ToolDef(
        name="list_directory",
        description="List the entries in a directory. Returns name / type / size / mtime per entry as JSON.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
            "additionalProperties": False,
        },
        handler=_h_list_directory,
        read_only_hint=True,
    ),
    ToolDef(
        name="list_processes",
        description="List running processes. Optionally filter by image-name substring.",
        tier="observe",
        input_schema={
            "type": "object",
            "properties": {"name_filter": {"type": "string"}},
            "additionalProperties": False,
        },
        handler=_h_list_processes,
        read_only_hint=True,
    ),
    ToolDef(
        name="get_clipboard",
        description="Read the clipboard's text content.",
        tier="observe",
        input_schema={"type": "object", "properties": {}, "additionalProperties": False},
        handler=_h_get_clipboard,
        read_only_hint=True,
    ),

    # ----- Drive -------------------------------------------------------------
    ToolDef(
        name="focus_window",
        description="Bring a window to the foreground. ERR `lock_held` means Windows refused — usually because no foreground process granted us permission. See `docs/windows-automation-notes.md`.",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {"hwnd": {"type": "string", "description": "win:0xHEX from list_windows / find_window"}},
            "required": ["hwnd"],
            "additionalProperties": False,
        },
        handler=_h_focus_window,
    ),
    ToolDef(
        name="close_window",
        description="Send WM_CLOSE to a window (graceful — the app may decline, e.g. unsaved-document prompt).",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {"hwnd": {"type": "string"}},
            "required": ["hwnd"],
            "additionalProperties": False,
        },
        handler=_h_close_window,
    ),
    ToolDef(
        name="click_element",
        description=(
            "Click a UIA element. Provide ONE of: `element_id` from a prior "
            "`find_element`/`list_elements`; `role`+`name_pattern` to find "
            "and click in one round-trip; or `x`+`y` to invoke the element "
            "at a screen coordinate. ERR `uipi_blocked` means the target "
            "window is at higher integrity than the agent — see "
            "`request_drive_access` / PROTOCOL.md §8."
        ),
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {
                "element_id": {"type": "string", "description": "elt:N from a prior find/list"},
                "role": {"type": "string"},
                "name_pattern": {"type": "string"},
                "x": {"type": "integer"},
                "y": {"type": "integer"},
            },
            "additionalProperties": False,
        },
        handler=_h_click_element,
    ),
    ToolDef(
        name="set_element_text",
        description="Replace the text in a UIA edit / text element via ValuePattern. Pair with find_element to locate the field first.",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {
                "element_id": {"type": "string"},
                "text": {"type": "string"},
            },
            "required": ["element_id", "text"],
            "additionalProperties": False,
        },
        handler=_h_set_element_text,
    ),
    ToolDef(
        name="type_text",
        description="Type literal Unicode text into the focused window via SendInput. Handles characters that would be hazardous to send as keystrokes.",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {"text": {"type": "string", "minLength": 1}},
            "required": ["text"],
            "additionalProperties": False,
        },
        handler=_h_type_text,
    ),
    ToolDef(
        name="press_keys",
        description="Press a named key (`enter`, `tab`, `F4`, `a`, …) with optional modifiers (`ctrl,shift`).",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {
                "key": {"type": "string"},
                "modifiers": {"type": "string", "description": "comma-separated: ctrl,alt,shift,win"},
            },
            "required": ["key"],
            "additionalProperties": False,
        },
        handler=_h_press_keys,
    ),
    ToolDef(
        name="click",
        description="Click at an absolute screen coordinate. Prefer `click_element` for UI buttons — coordinates drift across DPI / window-position changes.",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "default": "left"},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
        handler=_h_click_at,
    ),
    ToolDef(
        name="write_file",
        description="Write UTF-8 text to a file (overwrites). Binary writes need a different tool (TODO).",
        tier="drive",
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
        name="launch",
        description="CreateProcess on a command line. Returns the spawned PID. For paths with spaces / unicode prefer `shell_open`.",
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {"command_line": {"type": "string", "minLength": 1}},
            "required": ["command_line"],
            "additionalProperties": False,
        },
        handler=_h_launch,
    ),
    ToolDef(
        name="shell_open",
        description=(
            "ShellExecuteEx — opens a path/URL with the default-associated app. "
            "Use for `start http://...` or `start file.pdf` semantics. Pass "
            "`verb='runas'` to elevate (triggers UAC). Returns spawned PID "
            "(may be 0 for verbs that don't spawn a new process)."
        ),
        tier="drive",
        input_schema={
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "args": {"type": "string"},
                "verb": {"type": "string", "description": "open (default), runas, edit, print"},
            },
            "required": ["path"],
            "additionalProperties": False,
        },
        handler=_h_shell_open,
    ),

    # ----- Power -------------------------------------------------------------
    ToolDef(
        name="kill_process",
        description="TerminateProcess — abrupt, no save. For graceful shutdown try `close_window` first.",
        tier="power",
        input_schema={
            "type": "object",
            "properties": {"pid": {"type": "integer", "minimum": 1}},
            "required": ["pid"],
            "additionalProperties": False,
        },
        handler=_h_kill_process,
        destructive_hint=True,
    ),
    ToolDef(
        name="delete_file",
        description="Delete a file. Irreversible — no Recycle Bin.",
        tier="power",
        input_schema={
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
            "additionalProperties": False,
        },
        handler=_h_delete_file,
        destructive_hint=True,
    ),
    ToolDef(
        name="cancel_pending_shutdown",
        description="Abort a pending --delay shutdown / reboot / logoff scheduled via system.power. Returns ERR not_found if no shutdown is pending.",
        tier="power",
        input_schema={"type": "object", "properties": {}, "additionalProperties": False},
        handler=_h_cancel_pending_shutdown,
    ),
]


# ---------------------------------------------------------------------------
# Lookup helpers

def tools_by_tier(current_tier: str) -> list[ToolDef]:
    """Return tools the LLM should see at `current_tier`. `always` tools are
    always included; tiered tools are included if `current_tier` reaches them."""
    order = {"observe": 0, "drive": 1, "power": 2, "always": -1}
    cur = order.get(current_tier, 0)
    out: list[ToolDef] = []
    for t in TOOLS:
        if t.tier == "always" or order.get(t.tier, 99) <= cur:
            out.append(t)
    return out


def find_tool(name: str) -> Optional[ToolDef]:
    for t in TOOLS:
        if t.name == name:
            return t
    return None
