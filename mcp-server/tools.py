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
import socket
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from agent_client import AgentClient, ErrResponse, OkResponse, WireError, read_token_file


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
    # agent_client.request auto-appends len(payload) to the header — don't
    # pass the length explicitly here, or it'd be doubled on the wire.
    return _format_ok(_ok_or_raise(
        client.request("element.set_text", elt_id, payload=text),
        "element.set_text"))


def _h_type_text(args: dict, client: AgentClient) -> str:
    text = args["text"].encode("utf-8")
    # agent_client.request auto-appends len(payload) — see comment in
    # _h_set_element_text above.
    return _format_ok(_ok_or_raise(
        client.request("input.type", payload=text), "input.type"))


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
    """Write UTF-8 text content. The wire-level file.write is binary-safe;
    this handler narrows it to text by UTF-8 encoding the `content` arg.
    For binary content, use `write_file_b64` (LLM-supplied bytes) or
    `upload_file` (push from the controller's filesystem)."""
    path = args["path"]
    payload = args["content"].encode("utf-8")
    # agent_client.request auto-appends len(payload) — don't pass length
    # explicitly here, or the wire header gets it twice.
    return _format_ok(_ok_or_raise(
        client.request("file.write", path, payload=payload),
        "file.write"))


def _h_write_file_b64(args: dict, client: AgentClient) -> str:
    """Write binary content the LLM has in hand as a base64 string. Suited
    for small fixtures, generated content, or anything where the bytes are
    already in the LLM's context. For larger files, prefer `upload_file`
    so the bytes never pass through the LLM transport."""
    path = args["path"]
    try:
        payload = base64.b64decode(args["content_b64"], validate=True)
    except (ValueError, TypeError) as e:
        raise ValueError(f"content_b64 is not valid base64: {e}") from e
    return _format_ok(_ok_or_raise(
        client.request("file.write", path, payload=payload),
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
    """Send a single `file.write_at` request. Raises on protocol-level
    failure (ErrResponse) — caller handles transport-level failures.

    Note: agent_client.request() auto-appends len(payload) to the header,
    so we don't pass the length explicitly. The agent's file.write_at
    parser walks args looking for the optional --truncate flag and treats
    the last two non-flag tokens as offset and length, so the auto-appended
    length lands in the right slot regardless of where --truncate sits."""
    args_list = [dest, str(offset)]
    if truncate:
        args_list.append("--truncate")
    r = client.request("file.write_at", *args_list, payload=chunk)
    if isinstance(r, ErrResponse):
        raise WireError(f"file.write_at[{offset}+{len(chunk)}]: {r.code} {r.detail}")


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
        # agent_client.request auto-appends len(payload).
        _ok_or_raise(
            client.request("file.write", destination_path, payload=payload),
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

def _watch_subscription(client: AgentClient, verb: str, args: list[str],
                        timeout_ms: int, sub_id_in_result: bool = True) -> dict:
    """Common machinery for the wait-for-* tools. Subscribes via `verb` +
    `args`, waits up to `timeout_ms` for an EVENT, cancels, returns a dict
    suitable for inclusion in the tool result."""
    r = client.request(verb, *args)
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
        # Best-effort cancel. --until-change subscriptions auto-cancel on
        # fire, so this is a no-op for them on the success path; on timeout
        # or error it's needed to free the agent-side worker thread.
        try:
            client.request("watch.cancel", sub_id)
        except Exception:
            pass

    out: dict = {"subscription_id": sub_id, "timeout_ms": timeout_ms,
                 "timed_out": timed_out}
    if not sub_id_in_result:
        out.pop("subscription_id", None)
    return out, event_body


def _h_wait_for_visual_change(args: dict, client: AgentClient) -> str:
    """Wait for visual change in a screen region. Wraps
    `watch.region --until-change`. Returns the captured frame as a
    base64-encoded PNG when the change fires, or a timeout marker."""
    region = args["region"]
    interval_ms = int(args.get("interval_ms", 500))
    timeout_ms = int(args.get("timeout_ms", 30000))

    out, body = _watch_subscription(
        client, "watch.region",
        [region, "--interval", str(interval_ms), "--until-change"],
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
    appear (or disappear). Wraps `watch.window`. Returns the first
    `window_appeared` / `window_gone` event."""
    title_prefix = args["title_prefix"]
    timeout_ms = int(args.get("timeout_ms", 30000))

    out, body = _watch_subscription(
        client, "watch.window",
        ["--title-prefix", title_prefix],
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
    """Wait for a process to exit. Wraps `watch.process` (auto-cancels
    on exit). Returns the exit code via the agent's process_exit event."""
    pid = str(int(args["pid"]))
    timeout_ms = int(args.get("timeout_ms", 60000))

    out, body = _watch_subscription(
        client, "watch.process",
        [pid],
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
    """Wait for any file modification matching a path / glob pattern.
    Wraps `watch.file`. Returns the first change event (created, deleted,
    modified, renamed)."""
    pattern = args["pattern"]
    timeout_ms = int(args.get("timeout_ms", 60000))

    out, body = _watch_subscription(
        client, "watch.file",
        [pattern],
        timeout_ms,
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
            f"'{client.current_tier}'. Raise tier first via "
            f"request_drive_access / request_power_access.")
    return tool.handler(arguments, client)


def _can_satisfy_tier(required: str, current: str) -> bool:
    """True if a connection at `current` tier can call a tool requiring
    `required` tier."""
    order = {"always": 0, "observe": 1, "drive": 2, "power": 3}
    return order.get(current, -1) >= order.get(required, 99)


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
        tier="observe",
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
        tier="observe",
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
        tier="observe",
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
        tier="observe",
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
        description=(
            "Write UTF-8 text to a file on the target (overwrites). For "
            "binary content the LLM has in hand, use `write_file_b64`. For "
            "binary content from a path on the controller (zip, exe, DLLs, "
            "archives), use `upload_file` — bytes don't pass through MCP."
        ),
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
        name="write_file_b64",
        description=(
            "Write base64-encoded binary content to a file on the target "
            "(overwrites). Use when the LLM has the bytes in hand as a "
            "base64 string — typically generated content or small fixtures. "
            "For binary content from a path on the controller, prefer "
            "`upload_file` so the bytes never pass through the LLM context."
        ),
        tier="drive",
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
        name="upload_file",
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
            "content already in the LLM's context, use `write_file`."
        ),
        tier="drive",
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
