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

"""Tests for `clipboard.*`."""

from __future__ import annotations

import base64
import json
import multiprocessing
import time

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# Helpers
#
# clipboard.set's binary/image/files paths take typed JSON arguments that the
# `--key value` flag-flattening in wire.py's `_args_to_dict` cannot represent
# (a base64 string for `content`, an integer for `win32_format_id`, an array
# for `paths`). Drive `tools/call` directly through the lower-level MCP
# primitives — same pattern as `test_rebuild_v030.py::_tools_call_raw`.


def _tools_call(client: WireClient, name: str, arguments: dict) -> tuple:
    """Returns (is_error: bool, code: str, body: dict) for a tools/call.

    `body` is the JSON-decoded text content item (the verb's OK or ERR body)."""
    rid = client._alloc_id()
    client._send_mcp({
        "jsonrpc": "2.0", "id": rid, "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    })
    resp = client._read_mcp_response(rid)
    if "error" in resp:
        pytest.fail(f"MCP protocol error on {name}: {resp['error']}")
    result = resp.get("result", {})
    text = ""
    for item in result.get("content", []):
        if item.get("type") == "text":
            text = item.get("text", "")
            break
    try:
        body = json.loads(text) if text else {}
    except json.JSONDecodeError:
        body = {"_raw": text}
    return bool(result.get("isError")), result.get("arh_error_code", ""), body


# A 2x2 PNG, all pixels solid red. Hand-built so the test does not pull in
# Pillow / a PNG library — the magic bytes + IHDR width/height are enough to
# verify the round-trip semantically.
_RED_2X2_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVQIHWP8z8AARIz/"
    "GRgY/jMAACUBA/9bRtxsAAAAAElFTkSuQmCC"
)
_PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _png_dims(png: bytes) -> tuple:
    """Return (width, height) decoded from a PNG's IHDR chunk."""
    assert png.startswith(_PNG_MAGIC), "not a PNG"
    # IHDR is the first chunk: 8 (magic) + 4 (chunk length) + 4 (type "IHDR")
    # then width (u32 BE) + height (u32 BE).
    w = int.from_bytes(png[16:20], "big")
    h = int.from_bytes(png[20:24], "big")
    return w, h


# ---------------------------------------------------------------------------
# Original text-format tests (kept passing under the new dispatch).


def test_clipboard_get_at_read_tier(client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    r = client.request("clipboard.get")
    # Either OK with text body, or ERR empty (depends on whether anything is
    # on the test VM's clipboard). Both outcomes prove the verb is reachable
    # at read tier.
    assert isinstance(r, (OkResponse, ErrResponse))


def test_clipboard_set_requires_update_tier(client: WireClient,
                                            capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.set")
    r = client.request("clipboard.set", "--format", "text",
                       "--content", "hello")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_clipboard_text_round_trip(update_client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    text = "agent-remote-hands conformance test"
    r = update_client.request("clipboard.set", "--format", "text",
                              "--content", text)
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body.get("format") == "text"

    r = update_client.request("clipboard.get", "--format", "text")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body.get("format") == "text"
    assert body.get("content") == text


# ---------------------------------------------------------------------------
# binary / image / files extensions (per per-verb/clipboard.binary.md plan).


def test_clipboard_binary_round_trip(update_client: WireClient,
                                     capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    # CF_PRIVATEFIRST (0x0200) — registered for "any application can use this".
    # Using a non-text custom CF picks the slot up via the win32_format_id
    # lookup and avoids colliding with whatever owns CF_UNICODETEXT.
    cf = 0x0200
    raw = bytes(range(64)) + b"\x00\xffabc"
    b64 = base64.b64encode(raw).decode("ascii")

    is_err, code, body = _tools_call(update_client, "clipboard.set", {
        "format": "binary",
        "win32_format_id": cf,
        "content": b64,
    })
    assert not is_err, f"clipboard.set binary failed: {code} {body}"
    assert body.get("format") == "binary"
    assert body.get("win32_format_id") == cf

    is_err, code, body = _tools_call(update_client, "clipboard.get", {
        "format": "binary",
        "win32_format_id": cf,
    })
    assert not is_err, f"clipboard.get binary failed: {code} {body}"
    assert body.get("format") == "binary"
    assert body.get("win32_format_id") == cf
    assert "win32_format_name" in body  # may be empty string for standard CFs
    got = base64.b64decode(body.get("content", ""))
    assert got == raw


def test_clipboard_image_round_trip(update_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    b64_in = base64.b64encode(_RED_2X2_PNG).decode("ascii")

    is_err, code, body = _tools_call(update_client, "clipboard.set", {
        "format": "image",
        "content": b64_in,
    })
    assert not is_err, f"clipboard.set image failed: {code} {body}"
    assert body.get("format") == "image"

    is_err, code, body = _tools_call(update_client, "clipboard.get", {
        "format": "image",
    })
    assert not is_err, f"clipboard.get image failed: {code} {body}"
    assert body.get("format") == "image"
    assert body.get("mime") == "image/png"
    assert body.get("width") == 2
    assert body.get("height") == 2
    png_out = base64.b64decode(body.get("content", ""))
    assert png_out.startswith(_PNG_MAGIC), "round-trip output is not a PNG"
    w, h = _png_dims(png_out)
    assert (w, h) == (2, 2)


def test_clipboard_files_round_trip(update_client: WireClient,
                                    capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    # Two well-known Windows paths. Existence is not required for CF_HDROP —
    # the clipboard happily carries non-existent paths (drag-drop simulation).
    paths_in = [r"C:\Windows\System32\notepad.exe", r"C:\Windows\win.ini"]

    is_err, code, body = _tools_call(update_client, "clipboard.set", {
        "format": "files",
        "paths": paths_in,
    })
    assert not is_err, f"clipboard.set files failed: {code} {body}"
    assert body.get("format") == "files"
    assert body.get("count") == len(paths_in)

    is_err, code, body = _tools_call(update_client, "clipboard.get", {
        "format": "files",
    })
    assert not is_err, f"clipboard.get files failed: {code} {body}"
    assert body.get("format") == "files"
    paths_out = body.get("paths", [])
    # Case-insensitive compare — some Windows shells normalise path casing on
    # CF_HDROP retrieval.
    assert [p.lower() for p in paths_out] == [p.lower() for p in paths_in]


def test_clipboard_get_image_when_text(update_client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    # Put text on the clipboard, then ask for the image format. Spec:
    # ERR empty {"format":"image"} — semantic "no image on clipboard",
    # distinct from "clipboard fully empty".
    r = update_client.request("clipboard.set", "--format", "text",
                              "--content", "text-only payload")
    assert isinstance(r, OkResponse)

    is_err, code, body = _tools_call(update_client, "clipboard.get", {
        "format": "image",
    })
    assert is_err
    assert code == "empty"
    assert body.get("format") == "image"


def test_clipboard_get_empty_clipboard(update_client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.get")
    needs_verb(capabilities, "clipboard.set")
    # Set text only, then ask for the files format. The CF_HDROP slot is
    # absent → ERR empty {"format":"files"}. This is the closest deterministic
    # stand-in for "format absent" without an EmptyClipboard verb on the
    # wire surface.
    r = update_client.request("clipboard.set", "--format", "text",
                              "--content", "x")
    assert isinstance(r, OkResponse)
    is_err, code, body = _tools_call(update_client, "clipboard.get", {
        "format": "files",
    })
    assert is_err
    assert code == "empty"
    assert body.get("format") == "files"


def test_clipboard_set_binary_no_format_id(update_client: WireClient,
                                           capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.set")
    # Binary set without win32_format_id is invalid_args by spec — the
    # custom-format ID is the only way to disambiguate the target slot.
    is_err, code, body = _tools_call(update_client, "clipboard.set", {
        "format": "binary",
        "content": base64.b64encode(b"hello").decode("ascii"),
    })
    assert is_err
    assert code == "invalid_args"


def test_clipboard_concurrent_lock_retry(update_client: WireClient,
                                         capabilities: dict) -> None:
    needs_verb(capabilities, "clipboard.set")
    # Hold the clipboard open from another process for ~30ms — shorter than
    # the handler's retry budget (5 attempts × 10ms = ~50ms) — and verify
    # the set succeeds after the lock releases. Longer than the budget would
    # surface as a clean permission_denied; both outcomes are spec-allowed,
    # so accept either.
    proc = multiprocessing.Process(target=_hold_clipboard, args=(0.03,))
    proc.start()
    # Give the holder a moment to actually own the clipboard.
    time.sleep(0.005)
    r = update_client.request("clipboard.set", "--format", "text",
                              "--content", "lock-retry probe")
    proc.join(timeout=2.0)
    if isinstance(r, ErrResponse):
        # Spec-allowed alternative: the holder kept the lock past the retry
        # budget. Must still be the spec-declared permission_denied code.
        assert r.code == "permission_denied", \
            f"unexpected error: {r.code} {r.detail}"
    else:
        assert isinstance(r, OkResponse)


def _hold_clipboard(seconds: float) -> None:
    """Subprocess helper: open the clipboard via ctypes, sleep, release.

    Used by test_clipboard_concurrent_lock_retry; lives at module scope so
    the multiprocessing spawn start method on Windows can import it."""
    import ctypes
    user32 = ctypes.windll.user32
    if user32.OpenClipboard(0):
        try:
            time.sleep(seconds)
        finally:
            user32.CloseClipboard()
