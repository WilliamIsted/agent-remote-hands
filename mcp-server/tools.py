"""Tool definitions for the Agent Remote Hands MCP server.

Each tool is a `ToolDef` carrying:

  - `name`: the MCP tool name as Claude Code sees it
  - `description`: prose shown to the model. Mechanism #1 + #2 from the
    preference-conveying design — every "lower-tier" tool's description
    explicitly names the preferred alternative ("PREFER `click_element`
    when..."). The model's tool choice is shaped here as much as anywhere.
  - `schema`: JSON Schema describing the arguments
  - `annotations`: MCP spec metadata (mechanism #4) — `destructiveHint` on
    tools that mutate or destroy state, `readOnlyHint` on pure queries.
    Claude Code surfaces confirmations for destructive ops.
  - `requires_caps` / `requires_info`: capability gating. The tool isn't
    even listed if the connected agent doesn't advertise these in CAPS or
    INFO. Stops the model from trying things that will fail.
  - `handler`: async function `(client, args) -> list[Content]`.

Returning content: handlers return a list of MCP `Content` items. Most
return `[TextContent(type="text", text=...)]`; capture tools return
`[ImageContent(...)]` so the model sees the screenshot inline.
"""
from __future__ import annotations

import base64
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

from mcp.types import ImageContent, TextContent

from agent_client import AgentError, AsyncAgentClient


Content = TextContent | ImageContent
Handler = Callable[[AsyncAgentClient, dict[str, Any]], Awaitable[list[Content]]]


@dataclass
class ToolDef:
    name: str
    description: str
    schema: dict[str, Any]
    handler: Handler
    requires_caps: set[str] = field(default_factory=set)
    requires_info: dict[str, str] = field(default_factory=dict)
    # MCP spec annotations — `destructiveHint`, `readOnlyHint`,
    # `idempotentHint`, `openWorldHint`. Claude Code uses these to decide
    # whether to ask for confirmation.
    annotations: dict[str, Any] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Helpers used by handlers
# ---------------------------------------------------------------------------

def _text(s: str) -> list[Content]:
    return [TextContent(type="text", text=s)]


def _image(data: bytes, mime: str) -> list[Content]:
    return [ImageContent(type="image", data=base64.b64encode(data).decode(),
                         mimeType=mime)]


def _mime_for_format(fmt: str) -> str:
    if fmt == "png":
        return "image/png"
    if fmt.startswith("webp"):
        return "image/webp"
    return "image/bmp"


def _ok_or_raise(line: str) -> str:
    if not line.startswith("OK"):
        raise AgentError(line)
    return line


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

async def _h_take_screenshot(client: AsyncAgentClient, args: dict) -> list[Content]:
    fmt = args.get("format", "png")
    region = args.get("region")
    if region:
        cmd = f"SHOTRECT {fmt} {region['x']} {region['y']} {region['w']} {region['h']}"
    else:
        cmd = f"SHOT {fmt}"
    extras, data = await client.cmd_data(cmd)
    return _image(data, _mime_for_format(fmt))


async def _h_wait_for_change(client: AsyncAgentClient, args: dict) -> list[Content]:
    timeout_ms = int(args.get("timeout_ms", 10000))
    threshold = float(args.get("threshold_pct", 1.0))
    fmt = args.get("format", "png")
    cmd = f"WAITFOR {fmt} {timeout_ms} {threshold}"
    extras, data = await client.cmd_data(cmd)
    return _image(data, _mime_for_format(fmt))


# ---------------------------------------------------------------------------
# Pixel-based mouse + keyboard (lower tier — descriptions point at UIA)
# ---------------------------------------------------------------------------

async def _h_move_mouse(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"MOVE {int(args['x'])} {int(args['y'])}")
    return _text("ok")


async def _h_click(client: AsyncAgentClient, args: dict) -> list[Content]:
    if "x" in args and "y" in args:
        await client.cmd_ok(f"MOVE {int(args['x'])} {int(args['y'])}")
    button = args.get("button", "left")
    verb = "DCLICK" if args.get("double") else "CLICK"
    await client.cmd_ok(f"{verb} {button}")
    return _text("ok")


async def _h_drag(client: AsyncAgentClient, args: dict) -> list[Content]:
    button = args.get("button", "left")
    await client.cmd_ok(f"MOVE {int(args['from_x'])} {int(args['from_y'])}")
    await client.cmd_ok(f"DRAG {int(args['to_x'])} {int(args['to_y'])} {button}")
    return _text("ok")


async def _h_press_keys(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"KEY {args['combo']}")
    return _text("ok")


async def _h_type_text(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"KEYS {args['text']}")
    return _text("ok")


async def _h_scroll(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"WHEEL {int(args['delta'])}")
    return _text("ok")


# ---------------------------------------------------------------------------
# UI Automation (higher tier — preferred whenever the target is named)
# ---------------------------------------------------------------------------

async def _h_list_elements(client: AsyncAgentClient, args: dict) -> list[Content]:
    region = args.get("region")
    if region:
        cmd = f"ELEMENTS {region['x']} {region['y']} {region['w']} {region['h']}"
    else:
        cmd = "ELEMENTS"
    extras, data = await client.cmd_data(cmd)
    return _text(data.decode("latin-1"))


async def _h_find_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    role = args["role"]
    name = args.get("name_substring", "")
    line = await client.cmd(f"ELEMENT_FIND {role} {name}".rstrip())
    _ok_or_raise(line)
    return _text(line[3:])  # strip leading "OK "


async def _h_click_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    """Convenience: ELEMENT_FIND + ELEMENT_INVOKE."""
    role = args["role"]
    name = args.get("name_substring", "")
    line = await client.cmd(f"ELEMENT_FIND {role} {name}".rstrip())
    _ok_or_raise(line)
    eid = int(line[3:].split("\t")[0])
    await client.cmd_ok(f"ELEMENT_INVOKE {eid}")
    return _text(f"invoked element {eid} ({line[3:].rstrip()})")


async def _h_focus_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"ELEMENT_FOCUS {int(args['element_id'])}")
    return _text("ok")


async def _h_get_element_text(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data(f"ELEMENT_TEXT {int(args['element_id'])}")
    return _text(data.decode("latin-1"))


async def _h_set_element_text(client: AsyncAgentClient, args: dict) -> list[Content]:
    text = args["text"].encode("latin-1", errors="replace")
    await client.cmd_ok(
        f"ELEMENT_SET_TEXT {int(args['element_id'])} {len(text)}", text
    )
    return _text("ok")


async def _h_toggle_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"ELEMENT_TOGGLE {int(args['element_id'])}")
    return _text("ok")


async def _h_expand_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"ELEMENT_EXPAND {int(args['element_id'])}")
    return _text("ok")


async def _h_collapse_element(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"ELEMENT_COLLAPSE {int(args['element_id'])}")
    return _text("ok")


async def _h_element_at(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd(f"ELEMENT_AT {int(args['x'])} {int(args['y'])}")
    _ok_or_raise(line)
    return _text(line[3:])


async def _h_element_tree(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data(
        f"ELEMENT_TREE {int(args['element_id'])}"
    )
    return _text(data.decode("latin-1"))


# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

async def _h_read_file(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data(f"READ {args['path']}")
    encoding = args.get("encoding", "latin-1")
    if encoding == "binary":
        return _text(f"<binary, {len(data)} bytes>")
    return _text(data.decode(encoding, errors="replace"))


async def _h_write_file(client: AsyncAgentClient, args: dict) -> list[Content]:
    payload = args["content"].encode(args.get("encoding", "latin-1"))
    await client.cmd_ok(f"WRITE {args['path']} {len(payload)}", payload)
    return _text(f"wrote {len(payload)} bytes")


async def _h_list_directory(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data(f"LIST {args['path']}")
    return _text(data.decode("latin-1"))


async def _h_file_info(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd(f"STAT {args['path']}")
    _ok_or_raise(line)
    return _text(line[3:])


async def _h_delete(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"DELETE {args['path']}")
    return _text("deleted")


async def _h_make_directory(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"MKDIR {args['path']}")
    return _text("ok")


async def _h_rename(client: AsyncAgentClient, args: dict) -> list[Content]:
    # Use tab separator if either path contains a space — the agent
    # accepts either separator, but tab disambiguates.
    sep = "\t" if (" " in args["src"] or " " in args["dst"]) else " "
    await client.cmd_ok(f"RENAME {args['src']}{sep}{args['dst']}")
    return _text("ok")


# ---------------------------------------------------------------------------
# Process
# ---------------------------------------------------------------------------

async def _h_run(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data(f"RUN {args['command']}")
    exit_code = int(extras[0]) if extras else 0
    output = data.decode("latin-1", errors="replace")
    return _text(f"exit={exit_code}\n{output}")


async def _h_launch(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd_ok(f"EXEC {args['command']}")
    pid = int(line.split()[1])
    return _text(f"launched pid={pid}")


async def _h_list_processes(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data("PS")
    return _text(data.decode("latin-1"))


async def _h_kill_process(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"KILL {int(args['pid'])}")
    return _text("killed")


# ---------------------------------------------------------------------------
# Window management
# ---------------------------------------------------------------------------

async def _h_list_windows(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data("WINLIST")
    return _text(data.decode("latin-1"))


async def _h_focus_window(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"WINFOCUS {args['title']}")
    return _text("ok")


async def _h_close_window(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok(f"WINCLOSE {args['title']}")
    return _text("ok")


async def _h_move_window(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd_ok(
        f"WINMOVE {int(args['x'])} {int(args['y'])} {args['title']}"
    )
    return _text(line[3:])


# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------

async def _h_get_clipboard(client: AsyncAgentClient, args: dict) -> list[Content]:
    extras, data = await client.cmd_data("CLIPGET")
    return _text(data.decode("latin-1"))


async def _h_set_clipboard(client: AsyncAgentClient, args: dict) -> list[Content]:
    payload = args["text"].encode("latin-1", errors="replace")
    await client.cmd_ok(f"CLIPSET {len(payload)}", payload)
    return _text("ok")


async def _h_get_idle_seconds(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd_ok("IDLE")
    return _text(line.split()[1])


async def _h_lock_workstation(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok("LOCK")
    return _text("locked")


async def _h_agent_info(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = await client.cmd("INFO")
    return _text(line[3:] if line.startswith("OK ") else line)


# ---------------------------------------------------------------------------
# Escape hatch
# ---------------------------------------------------------------------------

async def _h_raw_command(client: AsyncAgentClient, args: dict) -> list[Content]:
    line = args["line"]
    payload = args.get("payload_hex")
    payload_bytes = bytes.fromhex(payload) if payload else None
    response = await client.cmd(line, payload_bytes)
    if response.startswith("OK ") and len(response.split()) >= 2:
        # Possible data response — try to read length-prefixed body.
        try:
            length = int(response.split()[-1])
            body = await client._recv_n(length)
            return _text(f"{response}\n[{length} bytes]\n"
                         + body.decode("latin-1", errors="replace"))
        except Exception:
            pass
    return _text(response)


async def _h_abort(client: AsyncAgentClient, args: dict) -> list[Content]:
    await client.cmd_ok("ABORT")
    return _text("abort signalled")


# ---------------------------------------------------------------------------
# Tool registry
# ---------------------------------------------------------------------------

ALL_TOOLS: list[ToolDef] = [
    # ----- Capture -----
    ToolDef(
        name="take_screenshot",
        description=(
            "Capture the screen (or a region of it) and return the image. "
            "Use this whenever you need to *see* the current state — after a "
            "click, after typing, after waiting for something to appear. "
            "DO NOT poll this in a loop to wait for a UI change; use "
            "`wait_for_change` instead — it's far more efficient and avoids "
            "burning bandwidth.\n"
            "\n"
            "Format choice: `png` is a sane default (lossless, ~5-30x smaller "
            "than raw bitmap). For high-rate streaming or token-cost-sensitive "
            "use, `webp:85` is ~5-10x smaller than PNG with no perceptible "
            "loss for screen content. `bmp` is uncompressed — only use if "
            "the agent doesn't list png/webp in image_formats."
        ),
        schema={
            "type": "object",
            "properties": {
                "region": {
                    "type": "object",
                    "description": "Optional rectangular region in physical screen pixels. Omit for full screen.",
                    "properties": {
                        "x": {"type": "integer"}, "y": {"type": "integer"},
                        "w": {"type": "integer"}, "h": {"type": "integer"},
                    },
                    "required": ["x", "y", "w", "h"],
                },
                "format": {
                    "type": "string",
                    "enum": ["bmp", "png", "webp", "webp:85", "webp:75", "webp:50"],
                    "default": "png",
                },
            },
        },
        annotations={"readOnlyHint": True},
        handler=_h_take_screenshot,
        requires_caps={"SHOT"},
    ),
    ToolDef(
        name="wait_for_change",
        description=(
            "Block until the screen changes by more than `threshold_pct` of "
            "pixels (default 1.0%), then return the changed frame — or time "
            "out cleanly if nothing changes. PREFER this over polling "
            "`take_screenshot` in a loop when waiting for a UI event "
            "(dialog appears, page loads, button enables). The cursor is "
            "ignored, so cursor movement alone never triggers a frame.\n"
            "\n"
            "Returns a screenshot like `take_screenshot` does, plus is "
            "abortable via `abort()` from another tool call."
        ),
        schema={
            "type": "object",
            "properties": {
                "timeout_ms": {"type": "integer", "default": 10000},
                "threshold_pct": {"type": "number", "default": 1.0,
                                  "description": "Min %% of changed pixels to trigger. 1.0 = ~10 of the 1024 thumbnail pixels."},
                "format": {"type": "string", "enum": ["png", "webp", "webp:85", "bmp"],
                           "default": "png"},
            },
        },
        annotations={"readOnlyHint": True},
        handler=_h_wait_for_change,
        requires_caps={"WAITFOR"},
    ),

    # ----- UI Automation (preferred over pixel-based) -----
    ToolDef(
        name="list_elements",
        description=(
            "Enumerate UI elements (buttons, edit fields, menu items, links, "
            "etc.) visible on screen. Returns a tab-separated table — each "
            "row carries an element id you can pass to `click_element_by_id`, "
            "`focus_element`, `get_element_text`, etc. Restrict to a region "
            "for speed when you know roughly where to look (typical: \"what's "
            "on this dialog?\").\n"
            "\n"
            "PREFER this + `click_element` over pixel-based `click(x,y)` "
            "whenever a target is named. Robust to DPI / theming / layout / "
            "language changes."
        ),
        schema={
            "type": "object",
            "properties": {
                "region": {
                    "type": "object",
                    "properties": {
                        "x": {"type": "integer"}, "y": {"type": "integer"},
                        "w": {"type": "integer"}, "h": {"type": "integer"},
                    },
                    "required": ["x", "y", "w", "h"],
                },
            },
        },
        annotations={"readOnlyHint": True},
        handler=_h_list_elements,
        requires_caps={"ELEMENTS"},
    ),
    ToolDef(
        name="find_element",
        description=(
            "Locate the first UI element matching a role and (optional) "
            "name substring. Faster than `list_elements` when you know what "
            "you're looking for — \"find the OK button\" is `find_element` "
            "with role='button' name_substring='OK'. Returns the element row "
            "(id, position, role, name, value, flags)."
        ),
        schema={
            "type": "object",
            "properties": {
                "role": {
                    "type": "string",
                    "description": "Lowercase role token: button, edit, menu, menuitem, checkbox, combobox, tab, tabitem, link, text, image, list, listitem, tree, treeitem, slider, window, dialog, ...",
                },
                "name_substring": {
                    "type": "string",
                    "description": "Optional case-insensitive substring of the element's accessible name. Omit to match any element of the role.",
                    "default": "",
                },
            },
            "required": ["role"],
        },
        annotations={"readOnlyHint": True},
        handler=_h_find_element,
        requires_caps={"ELEMENT_FIND"},
    ),
    ToolDef(
        name="click_element",
        description=(
            "Find a UI element by role + name and trigger its primary action "
            "(button click, link follow, menu item) — atomic find-then-invoke. "
            "STRONGLY PREFER this over `click(x, y)` for any named control. "
            "No simulated mouse movement involved — UI Automation invokes the "
            "control's action directly, so the click is guaranteed to land "
            "on the right target. Robust to DPI, theming, layout shift, "
            "language localisation."
        ),
        schema={
            "type": "object",
            "properties": {
                "role": {"type": "string", "description": "Lowercase role token (see find_element)."},
                "name_substring": {"type": "string", "default": ""},
            },
            "required": ["role"],
        },
        annotations={"destructiveHint": False},
        handler=_h_click_element,
        requires_caps={"ELEMENT_FIND", "ELEMENT_INVOKE"},
    ),
    ToolDef(
        name="focus_element",
        description="Give keyboard focus to a UI element by id (from list_elements/find_element).",
        schema={"type": "object", "properties": {"element_id": {"type": "integer"}},
                "required": ["element_id"]},
        handler=_h_focus_element,
        requires_caps={"ELEMENT_FOCUS"},
    ),
    ToolDef(
        name="get_element_text",
        description=(
            "Read the text content of a UI element. Tries TextPattern (rich "
            "edits, documents) first, falls back to ValuePattern (simple "
            "edit fields), falls back to the accessible name. PREFER this "
            "over OCR'ing a screenshot when reading the contents of an edit "
            "field, label, or document."
        ),
        schema={"type": "object", "properties": {"element_id": {"type": "integer"}},
                "required": ["element_id"]},
        annotations={"readOnlyHint": True},
        handler=_h_get_element_text,
        requires_caps={"ELEMENT_TEXT"},
    ),
    ToolDef(
        name="set_element_text",
        description=(
            "Set the text content of an editable control via UI Automation. "
            "STRONGLY PREFER this over `type_text` for filling forms, edit "
            "fields, password boxes, or anything with a known element id. "
            "Handles password fields, IME input, and locked-format controls "
            "correctly where synthesised KEYS would fail."
        ),
        schema={
            "type": "object",
            "properties": {
                "element_id": {"type": "integer"},
                "text": {"type": "string"},
            },
            "required": ["element_id", "text"],
        },
        handler=_h_set_element_text,
        requires_caps={"ELEMENT_SET_TEXT"},
    ),
    ToolDef(
        name="toggle_element",
        description="Toggle a checkbox or toggle button by element id.",
        schema={"type": "object", "properties": {"element_id": {"type": "integer"}},
                "required": ["element_id"]},
        handler=_h_toggle_element,
        requires_caps={"ELEMENT_TOGGLE"},
    ),
    ToolDef(
        name="expand_element",
        description="Expand a combo box, tree node, or expandable menu item by id.",
        schema={"type": "object", "properties": {"element_id": {"type": "integer"}},
                "required": ["element_id"]},
        handler=_h_expand_element,
        requires_caps={"ELEMENT_EXPAND"},
    ),
    ToolDef(
        name="collapse_element",
        description="Collapse a combo box, tree node, or expandable menu item by id.",
        schema={"type": "object", "properties": {"element_id": {"type": "integer"}},
                "required": ["element_id"]},
        handler=_h_collapse_element,
        requires_caps={"ELEMENT_COLLAPSE"},
    ),
    ToolDef(
        name="element_at",
        description=(
            "Hit-test: which UI element is at the given screen pixel? Useful "
            "for confirming \"what's at this coordinate?\" before invoking, "
            "or for figuring out the role/name of something you can see."
        ),
        schema={
            "type": "object",
            "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}},
            "required": ["x", "y"],
        },
        annotations={"readOnlyHint": True},
        handler=_h_element_at,
        requires_caps={"ELEMENT_AT"},
    ),
    ToolDef(
        name="list_element_tree",
        description=(
            "Walk the UI Automation subtree rooted at a given element id "
            "and return every descendant. CRITICAL for web content: "
            "`list_elements` walks from the desktop root and is intentionally "
            "shallow (interactable controls only) — it surfaces Edge / "
            "Chrome's outer chrome (tabs, address bar) but NOT the page "
            "DOM (links, headings, buttons inside the document).\n"
            "\n"
            "To find a button or link on a web page: (1) `list_elements` "
            "to find the browser top-level window's element id, (2) "
            "`list_element_tree` with that id to get the full subtree "
            "including the page document and its links / buttons / text. "
            "Each row carries a depth column; depth 0 is the root you "
            "passed in."
        ),
        schema={
            "type": "object",
            "properties": {"element_id": {"type": "integer"}},
            "required": ["element_id"],
        },
        annotations={"readOnlyHint": True},
        handler=_h_element_tree,
        requires_caps={"ELEMENT_TREE"},
    ),

    # ----- Pixel-based mouse + keyboard (LAST RESORT — descriptions point at UIA) -----
    ToolDef(
        name="move_mouse",
        description=(
            "Move the mouse cursor to a screen pixel coordinate. PREFER "
            "`click_element` for clicking on named UI controls — that "
            "doesn't move the mouse, doesn't depend on coordinates, and is "
            "robust to DPI / theming / layout. Use `move_mouse` only when "
            "you genuinely need cursor positioning (drag operations, hover-"
            "to-reveal, or interacting with a canvas)."
        ),
        schema={
            "type": "object",
            "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}},
            "required": ["x", "y"],
        },
        handler=_h_move_mouse,
        requires_caps={"MOVE"},
    ),
    ToolDef(
        name="click",
        description=(
            "Click at a screen pixel coordinate. STRONGLY PREFER "
            "`click_element` whenever the target is a named UI control "
            "(button, link, menu item). Use this tool only for: "
            "(1) drawing canvases / image surfaces, "
            "(2) in-browser page content (HTML elements aren't UIA-visible), "
            "(3) custom-drawn controls without accessibility metadata, "
            "(4) games and full-screen Direct3D apps. "
            "Pixel coordinates are physical pixels (DPI-correct). Default "
            "button is left."
        ),
        schema={
            "type": "object",
            "properties": {
                "x": {"type": "integer"}, "y": {"type": "integer"},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "default": "left"},
                "double": {"type": "boolean", "default": False},
            },
        },
        handler=_h_click,
        requires_caps={"CLICK"},
    ),
    ToolDef(
        name="drag",
        description=(
            "Drag from one pixel to another with a held mouse button. "
            "PREFER UI-Automation patterns where they exist (e.g. SliderPattern "
            "for slider controls — though that's not yet exposed as an MCP "
            "tool). For the typical case (drag-and-drop in file managers, "
            "rearranging items, pulling a window), pixel drag is the right "
            "answer."
        ),
        schema={
            "type": "object",
            "properties": {
                "from_x": {"type": "integer"}, "from_y": {"type": "integer"},
                "to_x": {"type": "integer"}, "to_y": {"type": "integer"},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "default": "left"},
            },
            "required": ["from_x", "from_y", "to_x", "to_y"],
        },
        handler=_h_drag,
        requires_caps={"DRAG"},
    ),
    ToolDef(
        name="press_keys",
        description=(
            "Press a key or chord. Examples: `enter`, `tab`, `esc`, "
            "`alt-F4`, `ctrl-shift-s`. Separator is `-` or `+`. PREFER "
            "`click_element` for triggering button-equivalent actions; "
            "use `press_keys` for keyboard shortcuts (Ctrl+S, Alt+Tab) "
            "and navigation keys (Tab, Esc, arrows)."
        ),
        schema={
            "type": "object",
            "properties": {"combo": {"type": "string"}},
            "required": ["combo"],
        },
        handler=_h_press_keys,
        requires_caps={"KEY"},
    ),
    ToolDef(
        name="type_text",
        description=(
            "Type literal text into whatever control currently has focus. "
            "STRONGLY PREFER `set_element_text` for filling form fields, "
            "edit boxes, password fields — that uses the UI Automation API "
            "directly and handles edge cases (passwords, IME, locked formats) "
            "that synthesised typing breaks on. Use `type_text` only when no "
            "element id is available (in-browser fields, custom-drawn editors)."
        ),
        schema={
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
        handler=_h_type_text,
        requires_caps={"KEYS"},
    ),
    ToolDef(
        name="scroll",
        description=(
            "Scroll the wheel. Positive delta scrolls up, negative scrolls "
            "down; ±120 is one notch. Affects whatever's under the cursor."
        ),
        schema={
            "type": "object",
            "properties": {"delta": {"type": "integer"}},
            "required": ["delta"],
        },
        handler=_h_scroll,
        requires_caps={"WHEEL"},
    ),

    # ----- Files -----
    ToolDef(
        name="read_file",
        description=(
            "Read a file from the target machine and return its contents. "
            "Use `encoding='binary'` to skip decoding (returns size only — "
            "the model usually doesn't want raw bytes). Default encoding is "
            "Latin-1 to match the wire protocol; pass `utf-8` for explicit "
            "UTF-8 files."
        ),
        schema={
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "encoding": {"type": "string", "default": "latin-1",
                             "enum": ["latin-1", "utf-8", "ascii", "binary"]},
            },
            "required": ["path"],
        },
        annotations={"readOnlyHint": True},
        handler=_h_read_file,
        requires_caps={"READ"},
    ),
    ToolDef(
        name="write_file",
        description="Write a file to the target machine. Creates or replaces.",
        schema={
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "content": {"type": "string"},
                "encoding": {"type": "string", "default": "latin-1",
                             "enum": ["latin-1", "utf-8", "ascii"]},
            },
            "required": ["path", "content"],
        },
        annotations={"destructiveHint": True},
        handler=_h_write_file,
        requires_caps={"WRITE"},
    ),
    ToolDef(
        name="list_directory",
        description="List the contents of a directory. Returns tab-separated rows: type, size, mtime, name.",
        schema={"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
        annotations={"readOnlyHint": True},
        handler=_h_list_directory,
        requires_caps={"LIST"},
    ),
    ToolDef(
        name="file_info",
        description="Stat a file or directory. Returns type (F/D/L), size in bytes, modification time as Unix epoch.",
        schema={"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
        annotations={"readOnlyHint": True},
        handler=_h_file_info,
        requires_caps={"STAT"},
    ),
    ToolDef(
        name="delete_file",
        description="Delete a file or empty directory. THIS IS DESTRUCTIVE — only call after confirming the path is what you intend.",
        schema={"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
        annotations={"destructiveHint": True, "idempotentHint": True},
        handler=_h_delete,
        requires_caps={"DELETE"},
    ),
    ToolDef(
        name="make_directory",
        description="Create a directory (single level — parent must already exist).",
        schema={"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
        handler=_h_make_directory,
        requires_caps={"MKDIR"},
    ),
    ToolDef(
        name="rename",
        description="Rename or move a file or directory.",
        schema={
            "type": "object",
            "properties": {"src": {"type": "string"}, "dst": {"type": "string"}},
            "required": ["src", "dst"],
        },
        handler=_h_rename,
        requires_caps={"RENAME"},
    ),

    # ----- Process -----
    ToolDef(
        name="run",
        description=(
            "Run a command synchronously and return its stdout+stderr "
            "combined plus exit code. Blocks until the command finishes. "
            "PREFER `launch` for processes you want to keep running (GUIs, "
            "servers, daemons). Output is capped at 16 MB; further bytes "
            "are silently dropped. Use cmd /c \"...\" for shell features "
            "(pipes, redirection)."
        ),
        schema={"type": "object", "properties": {"command": {"type": "string"}},
                "required": ["command"]},
        annotations={"destructiveHint": True, "openWorldHint": True},
        handler=_h_run,
        requires_caps={"RUN"},
    ),
    ToolDef(
        name="launch",
        description=(
            "Launch a process asynchronously; returns the PID immediately. "
            "PREFER this for GUI applications, long-running daemons, or "
            "anything you want to outlive the tool call. Use `run` for "
            "short commands where you want the output."
        ),
        schema={"type": "object", "properties": {"command": {"type": "string"}},
                "required": ["command"]},
        annotations={"destructiveHint": True, "openWorldHint": True},
        handler=_h_launch,
        requires_caps={"EXEC"},
    ),
    ToolDef(
        name="list_processes",
        description="List running processes. Returns tab-separated rows: pid, image name.",
        schema={"type": "object", "properties": {}},
        annotations={"readOnlyHint": True},
        handler=_h_list_processes,
        requires_caps={"PS"},
    ),
    ToolDef(
        name="kill_process",
        description="Forcibly terminate a process by PID. THIS IS DESTRUCTIVE — confirm the target before invoking.",
        schema={"type": "object", "properties": {"pid": {"type": "integer"}},
                "required": ["pid"]},
        annotations={"destructiveHint": True, "idempotentHint": True},
        handler=_h_kill_process,
        requires_caps={"KILL"},
    ),

    # ----- Window management -----
    ToolDef(
        name="list_windows",
        description="List all visible top-level windows. Returns tab-separated rows: hwnd, x, y, w, h, title.",
        schema={"type": "object", "properties": {}},
        annotations={"readOnlyHint": True},
        handler=_h_list_windows,
        requires_caps={"WINLIST"},
    ),
    ToolDef(
        name="focus_window",
        description="Bring a window to the foreground (matched by title prefix).",
        schema={"type": "object", "properties": {"title": {"type": "string"}},
                "required": ["title"]},
        handler=_h_focus_window,
        requires_caps={"WINFOCUS"},
    ),
    ToolDef(
        name="close_window",
        description="Send WM_CLOSE to a window (matched by title prefix). The window may decline.",
        schema={"type": "object", "properties": {"title": {"type": "string"}},
                "required": ["title"]},
        annotations={"destructiveHint": True},
        handler=_h_close_window,
        requires_caps={"WINCLOSE"},
    ),
    ToolDef(
        name="move_window",
        description="Move a window (matched by title prefix) to (x, y).",
        schema={
            "type": "object",
            "properties": {
                "x": {"type": "integer"}, "y": {"type": "integer"},
                "title": {"type": "string"},
            },
            "required": ["x", "y", "title"],
        },
        handler=_h_move_window,
        requires_caps={"WINMOVE"},
    ),

    # ----- System -----
    ToolDef(
        name="get_clipboard",
        description=(
            "Read the system clipboard text content. PREFER this over "
            "`get_element_text` when reading content the user just copied; "
            "the clipboard is a more reliable channel for cross-app data "
            "transfer than reading element values."
        ),
        schema={"type": "object", "properties": {}},
        annotations={"readOnlyHint": True},
        handler=_h_get_clipboard,
        requires_caps={"CLIPGET"},
    ),
    ToolDef(
        name="set_clipboard",
        description=(
            "Set the system clipboard text. PREFER this over `type_text` for "
            "transferring large strings, paths, or code into a target app — "
            "it's instant, lossless, and survives focus changes. Then use "
            "`press_keys('ctrl-v')` (or the platform paste shortcut) to paste."
        ),
        schema={"type": "object", "properties": {"text": {"type": "string"}},
                "required": ["text"]},
        annotations={"destructiveHint": True},
        handler=_h_set_clipboard,
        requires_caps={"CLIPSET"},
    ),
    ToolDef(
        name="get_idle_seconds",
        description="Seconds since the user last interacted with the machine. Use to avoid stomping on an active human.",
        schema={"type": "object", "properties": {}},
        annotations={"readOnlyHint": True},
        handler=_h_get_idle_seconds,
        requires_caps={"IDLE"},
    ),
    ToolDef(
        name="lock_workstation",
        description="Lock the workstation (Windows+L equivalent). User has to log in again.",
        schema={"type": "object", "properties": {}},
        annotations={"destructiveHint": True},
        handler=_h_lock_workstation,
        requires_caps={"LOCK"},
    ),
    ToolDef(
        name="agent_info",
        description=(
            "Return the agent's INFO line — capability flags, OS, image "
            "formats supported, etc. Use this to understand what the agent "
            "can do; the MCP tool list is already filtered by these flags, "
            "but the raw INFO is useful for debugging or for adapting "
            "behaviour (e.g. picking a format)."
        ),
        schema={"type": "object", "properties": {}},
        annotations={"readOnlyHint": True},
        handler=_h_agent_info,
        requires_caps={"INFO"},
    ),

    # ----- Escape hatch -----
    ToolDef(
        name="raw_command",
        description=(
            "Send an arbitrary wire-protocol line to the agent. ESCAPE HATCH "
            "for verbs not exposed as named tools — use named tools when "
            "they exist. Pass binary payloads via `payload_hex` (hex-encoded). "
            "See PROTOCOL.md for the wire format."
        ),
        schema={
            "type": "object",
            "properties": {
                "line": {"type": "string", "description": "The full command line, e.g. 'SHOT png' or 'WAIT 1234 5000'."},
                "payload_hex": {"type": "string", "description": "Optional hex-encoded binary payload (for WRITE / CLIPSET / etc.)."},
            },
            "required": ["line"],
        },
        annotations={"openWorldHint": True},
        handler=_h_raw_command,
    ),
    ToolDef(
        name="abort",
        description=(
            "Cancel all in-flight long-running commands across every "
            "connection to the agent (RUN, SLEEP, WATCH, WAITFOR, blocking "
            "WAIT). The cancelled tools return ERR aborted to their callers; "
            "the agent itself stays running. Use when something is taking "
            "too long or you realised mid-action that you want to back out."
        ),
        schema={"type": "object", "properties": {}},
        handler=_h_abort,
        requires_caps={"ABORT"},
    ),
]


def filter_tools(caps: set[str], info: dict[str, str]) -> list[ToolDef]:
    """Capability-gated tool list. Only tools whose required CAPS verbs and
    INFO flags are advertised by the connected agent are returned. The model
    never sees a tool the agent can't run — keeps the surface small and
    avoids predictable failures."""
    out = []
    for t in ALL_TOOLS:
        if t.requires_caps and not t.requires_caps.issubset(caps):
            continue
        if any(info.get(k) != v for k, v in t.requires_info.items()):
            continue
        out.append(t)
    return out
