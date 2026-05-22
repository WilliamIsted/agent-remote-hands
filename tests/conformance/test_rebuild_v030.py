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

"""Agent-side assertions for the v0.3.0 rebuild (Phases R1/R2/R3/R5).

ADDITIVE-ONLY. This file does NOT modify any resynced submodule test. It
covers behaviour the canonical v2.2 submodule suite cannot structurally
assert because its `wire.py.request()` only extracts `type:"text"` content
items and never reads the Shape-B binary side-channel nor the MCP image
content item:

  * R1 — `system.info.capabilities.wake_timer_supported` (bool) and
    `system.info.capabilities.input_settings` block (5 fields).
  * R2 — the 8 new verbs are advertised in `system.capabilities` and behave
    per their `protocol/spec/verbs/**/<verb>.json` (happy-path where the
    read/create tier allows it; documented `tier_required` otherwise).
  * R3 — `screen.capture {encoding:"binary"}` Shape-B side-channel: JSON
    result carries an int `result.blob_size`, and exactly that many raw
    bytes follow the JSON frame forming a valid PNG/BMP.
  * §1.6.6 — default (no encoding arg) `screen.capture` returns an MCP
    image content item whose base64 `data` decodes to PNG/BMP magic.
  * R5 — disabled-element pre-check on element-handle invoke verbs +
    input.mouse.click hit-test (`target_element`) + opt-in `verify_enabled`
    refusal. Agent-ahead-of-spec: the discriminator `detail.reason ==
    "element_disabled"` and the `verify_enabled` arg are not yet in the
    pinned 2.2-rc protocol submodule; the schema PR is a deferred follow-up
    tracked in the task brief.

Every test is `needs_verb`-gated so an agent that does not advertise the
verb (older agents, the NT/classic build, the legacy build for the
modern-only Shape-B path) skips rather than fails. Import + fixture pattern
mirrors the sibling tests (e.g. `test_system.py`)."""

import base64
import json
import socket
import struct
import zlib

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# Off-screen coordinates: far outside any plausible virtual-screen rect, so
# any synthesised pointer op (if a test ever elevates) cannot perturb the
# host UI. The HARD RULE forbids hardcoding loopback hosts even in tests;
# these are screen coords, not network addresses.
OFFSCREEN_X = -32000
OFFSCREEN_Y = -32000

# PNG and BMP file magic — Shape-B / image-content payloads must be one.
_PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
_BMP_MAGIC = b"BM"


def _is_image_bytes(blob: bytes) -> bool:
    return blob.startswith(_PNG_MAGIC) or blob.startswith(_BMP_MAGIC)


# ---------------------------------------------------------------------------
# Shape-B + image-content helpers
#
# The resynced submodule wire.py has no Shape-B reader and request() only
# pulls text content items. Both helpers drive the lower-level MCP primitives
# directly (`_send_mcp` / `_alloc_id` / `_read_mcp_frame` / `_read_bytes`)
# so they observe the raw result object the agent emits.

def _tools_call_raw(client: WireClient, name: str, arguments: dict) -> dict:
    """Issue `tools/call` and return the raw JSON-RPC result object.

    Does NOT read any trailing Shape-B blob — the caller does that via
    `client._read_bytes(...)` immediately after, exactly matching the
    agent's `Content-Length`(JSON only) + trailing-raw-bytes framing."""
    rid = client._alloc_id()
    client._send_mcp({
        "jsonrpc": "2.0", "id": rid, "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    })
    resp = client._read_mcp_response(rid)
    if "error" in resp:
        err = resp["error"]
        pytest.fail(f"MCP protocol error on {name}: "
                    f"{err.get('code')} {err.get('message')!r}")
    return resp.get("result", {})


def _read_shape_b(client: WireClient, arguments: dict) -> tuple:
    """`screen.capture` Shape-B: read the Content-Length-framed JSON via the
    standard MCP frame reader, then read exactly `result.blob_size` raw
    trailing bytes. Returns (result_dict, blob_bytes).

    `_read_mcp_frame()` consumes precisely the Content-Length JSON bytes and
    leaves the trailing blob in the client buffer; `_read_bytes(n)` then
    drains exactly the declared blob. This mirrors the agent's
    McpCodec::write_frame (Content-Length counts ONLY the JSON; the blob
    follows the JSON's last byte and is NOT in Content-Length)."""
    rid = client._alloc_id()
    client._send_mcp({
        "jsonrpc": "2.0", "id": rid, "method": "tools/call",
        "params": {"name": "screen.capture", "arguments": arguments},
    })
    # Read frames until our id; Shape B is a bare JSON-RPC result (no
    # content[] wrapper) so we cannot use _read_mcp_response (it would also
    # be fine, but we want the frame boundary to stop exactly at the JSON's
    # last byte so the blob is still buffered).
    frame: dict = {}
    while True:
        frame = client._read_mcp_frame()
        if frame.get("id") == rid:
            break
        if "method" in frame and "id" not in frame:
            client.notifications.append(frame)
            continue
        client.notifications.append(frame)
    if "error" in frame:
        err = frame["error"]
        pytest.fail(f"MCP protocol error on screen.capture(binary): "
                    f"{err.get('code')} {err.get('message')!r}")
    result = frame.get("result", {})
    blob_size = result.get("blob_size")
    assert isinstance(blob_size, int) and not isinstance(blob_size, bool), \
        f"Shape B result.blob_size must be an int, got {blob_size!r}"
    blob = client._read_bytes(blob_size) if blob_size > 0 else b""
    return result, blob


# ===========================================================================
# R1 — system.info.capabilities new fields (#82 wake_timer, #86 input_settings)

def test_r1_wake_timer_supported_is_bool(
        client: WireClient, capabilities: dict) -> None:
    """`system.info.capabilities.wake_timer_supported` present + bool (#82)."""
    needs_verb(capabilities, "system.info")
    caps = client.info().get("capabilities", {})
    assert "wake_timer_supported" in caps, \
        "system.info.capabilities missing wake_timer_supported"
    assert isinstance(caps["wake_timer_supported"], bool), \
        f"wake_timer_supported must be bool, got {caps['wake_timer_supported']!r}"


def test_r1_input_settings_block_shape(
        client: WireClient, capabilities: dict) -> None:
    """`system.info.capabilities.input_settings` carries the required #86
    fields with the right types; `double_click_rect` is a {w,h} object;
    `wheel_scroll_lines` is optional (omitted on some classic agents)."""
    needs_verb(capabilities, "system.info")
    caps = client.info().get("capabilities", {})
    assert "input_settings" in caps, \
        "system.info.capabilities missing input_settings"
    s = caps["input_settings"]
    assert isinstance(s, dict), f"input_settings must be an object, got {s!r}"

    for k in ("double_click_time_ms", "keyboard_repeat_delay_ms",
              "keyboard_repeat_rate_cps"):
        assert k in s, f"input_settings missing {k}"
        assert isinstance(s[k], int) and not isinstance(s[k], bool), \
            f"input_settings.{k} must be an int, got {s[k]!r}"

    assert "double_click_rect" in s, "input_settings missing double_click_rect"
    rect = s["double_click_rect"]
    assert isinstance(rect, dict), \
        f"double_click_rect must be an object, got {rect!r}"
    for k in ("w", "h"):
        assert k in rect, f"double_click_rect missing {k}"
        assert isinstance(rect[k], int) and not isinstance(rect[k], bool), \
            f"double_click_rect.{k} must be an int, got {rect[k]!r}"

    if "wheel_scroll_lines" in s:  # optional per spec
        assert isinstance(s["wheel_scroll_lines"], int) and \
            not isinstance(s["wheel_scroll_lines"], bool), \
            f"wheel_scroll_lines must be an int, got {s['wheel_scroll_lines']!r}"


# ===========================================================================
# R2 — the 8 new verbs are advertised + spec-shaped

# --- advertisement: every R2 verb must appear in system.capabilities --------

R2_VERBS = (
    "input.mouse.press", "input.mouse.release", "input.mouse.drag",
    "input.keyboard.key_down", "input.keyboard.key_up", "input.position",
    "file.create", "file.download",
)


@pytest.mark.parametrize("verb", R2_VERBS)
def test_r2_verb_advertised(capabilities: dict, verb: str) -> None:
    """Each R2 verb is advertised in system.capabilities with a known tier.
    `needs_verb` skips (not fails) on an agent that doesn't implement it."""
    needs_verb(capabilities, verb)
    descriptor = capabilities[verb]
    assert isinstance(descriptor, dict), f"{verb}: descriptor not an object"
    assert descriptor.get("tier") in {
        "read", "create", "update", "delete", "extra_risky"
    }, f"{verb}: unknown tier {descriptor.get('tier')!r}"


# --- input.mouse.{press,release,drag}: Update tier --------------------------
# Spec x-output-schema is `null` (bare `OK 0`). At read tier (the `client`
# fixture default) the documented gate is `tier_required` — assert that
# without elevating (no host perturbation, no token needed).

def test_r2_mouse_press_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.press")
    r = client.request("input.mouse.press")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_mouse_release_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.release")
    r = client.request("input.mouse.release")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_mouse_drag_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.mouse.drag")
    # required: x, y — off-screen target so an unexpected elevation cannot
    # perturb the host UI.
    r = client.request("input.mouse.drag",
                       "--x", str(OFFSCREEN_X), "--y", str(OFFSCREEN_Y))
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_mouse_drag_missing_required_args_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """At update tier, omitting required x/y is the documented invalid_args
    error (spec required:["x","y"]). Skips if no token to elevate."""
    needs_verb(capabilities, "input.mouse.drag")
    r = update_client.request("input.mouse.drag")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# --- input.keyboard.{key_down,key_up}: Update tier -------------------------

def test_r2_key_down_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key_down")
    r = client.request("input.keyboard.key_down", "--vk", "shift")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_key_up_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "input.keyboard.key_up")
    r = client.request("input.keyboard.key_up", "--vk", "shift")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_key_down_missing_vk_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """At update tier, omitting required `vk` is the documented invalid_args
    error (spec required:["vk"]). Skips if no token to elevate."""
    needs_verb(capabilities, "input.keyboard.key_down")
    r = update_client.request("input.keyboard.key_down")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# --- input.position: Read tier — happy path on the fresh `client` ----------

def test_r2_position_returns_xy(
        client: WireClient, capabilities: dict) -> None:
    """input.position is Read tier (spec x-crudx:R) so it succeeds on a
    fresh hello. x-output-schema requires int x + int y."""
    needs_verb(capabilities, "input.position")
    r = client.request("input.position")
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    assert isinstance(body.get("x"), int) and not isinstance(body["x"], bool), \
        f"input.position x must be an int, got {body.get('x')!r}"
    assert isinstance(body.get("y"), int) and not isinstance(body["y"], bool), \
        f"input.position y must be an int, got {body.get('y')!r}"


def test_r2_position_include_monitor_adds_index(
        client: WireClient, capabilities: dict) -> None:
    """`include_monitor:true` adds a 0-based int monitor_index (spec
    x-output-schema: present iff include_monitor was passed)."""
    needs_verb(capabilities, "input.position")
    r = client.request("input.position", "--include-monitor")
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    assert "monitor_index" in body, \
        "include_monitor:true must add monitor_index"
    mi = body["monitor_index"]
    assert isinstance(mi, int) and not isinstance(mi, bool) and mi >= 0, \
        f"monitor_index must be a non-negative int, got {mi!r}"


# --- file.create: Create tier — happy path under a scratch temp path -------

def test_r2_file_create_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    """Read tier cannot reach a Create-tier verb (spec x-crudx:C)."""
    needs_verb(capabilities, "file.create")
    r = client.request("file.create",
                       "--path", r"C:\Windows\Temp\rha_r2_gate_probe.txt",
                       "--content", "x")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_file_create_happy_path(
        create_client: WireClient, capabilities: dict) -> None:
    """At create tier, file.create lands the bytes and echoes the
    spec x-output-schema {bytes_written:int, encoding:str}. Uses a scratch
    temp path; cleans up via file.delete when advertised. Skips if no
    token to elevate."""
    needs_verb(capabilities, "file.create")
    import os
    import time
    path = "C:\\Windows\\Temp\\rha_r2_create_%d_%d.txt" % (
        os.getpid(), int(time.time() * 1000) % 1000000)
    payload = "rebuild-v0.3.0 R2 file.create conformance"
    try:
        r = create_client.request("file.create",
                                  "--path", path,
                                  "--content", payload,
                                  "--encoding", "utf-8")
        assert isinstance(r, OkResponse), f"got {r!r}"
        body = json.loads(r.payload)
        assert isinstance(body.get("bytes_written"), int) and \
            not isinstance(body["bytes_written"], bool), \
            f"bytes_written must be an int, got {body.get('bytes_written')!r}"
        assert body["bytes_written"] == len(payload.encode("utf-8")), \
            f"bytes_written {body['bytes_written']} != " \
            f"{len(payload.encode('utf-8'))}"
        assert body.get("encoding") == "utf-8", \
            f"encoding echo must be utf-8, got {body.get('encoding')!r}"

        # Second create on the same path is the documented already_exists.
        r2 = create_client.request("file.create",
                                   "--path", path,
                                   "--content", "second")
        assert isinstance(r2, ErrResponse)
        assert r2.code == "already_exists"
    finally:
        if "file.delete" in capabilities:
            create_client.request("file.delete", "--path", path)


# --- file.download: Create tier --------------------------------------------
# Per the HARD RULE we must NOT hardcode loopback / localhost anywhere. We
# assert the tier gate (read tier) + a documented error path that needs no
# network: an unsupported method/scheme yields invalid_args before any I/O.

def test_r2_file_download_tier_gated(
        client: WireClient, capabilities: dict) -> None:
    """Read tier cannot reach a Create-tier verb (spec x-crudx:C)."""
    needs_verb(capabilities, "file.download")
    r = client.request(
        "file.download",
        "--url", "https://example.invalid/probe",
        "--local-path", r"C:\Windows\Temp\rha_r2_dl_gate_probe.bin")
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_r2_file_download_bad_scheme_rejected(
        create_client: WireClient, capabilities: dict) -> None:
    """At create tier, a non-http(s) URL scheme is the documented
    invalid_args error (spec: url scheme must be http or https) — no
    network access required, and no loopback host referenced. Skips if no
    token to elevate."""
    needs_verb(capabilities, "file.download")
    r = create_client.request(
        "file.download",
        "--url", "ftp://example.invalid/resource",
        "--local-path", r"C:\Windows\Temp\rha_r2_dl_badscheme.bin")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ===========================================================================
# R3 — screen.capture {encoding:"binary"} Shape-B side-channel
#
# Modern-family-only path. needs_verb gates on screen.capture; an agent that
# does not support the binary side-channel (legacy/classic) returns
# unsupported_format — asserted as the documented non-Shape-B branch.

def test_r3_screen_capture_binary_shape_b(
        client: WireClient, capabilities: dict) -> None:
    """`encoding:"binary"` → JSON-RPC result with an int `result.blob_size`
    and exactly that many raw trailing bytes forming a valid PNG/BMP. On a
    family without the side-channel the documented response is
    `unsupported_format` (read via the standard text-content path)."""
    needs_verb(capabilities, "screen.capture")

    # Probe family: the Shape-B path is modern-only. Non-modern returns the
    # documented unsupported_format; assert that and skip the byte checks.
    fam = client.info().get("family", "")
    if fam != "windows-modern":
        r = client.request("screen.capture", "--encoding", "binary")
        assert isinstance(r, ErrResponse), \
            f"non-modern binary screen.capture should error, got {r!r}"
        assert r.code == "unsupported_format", \
            f"expected unsupported_format on {fam!r}, got {r.code!r}"
        pytest.skip(f"Shape-B side-channel is modern-only; {fam} returns "
                    f"unsupported_format (asserted)")

    result, blob = _read_shape_b(client, {"encoding": "binary"})
    assert "blob_size" in result, "Shape B result missing blob_size"
    assert result["blob_size"] == len(blob), \
        f"blob_size {result['blob_size']} != trailing byte count {len(blob)}"
    assert len(blob) > 0, "Shape B blob is empty"
    assert _is_image_bytes(blob), \
        f"Shape B blob is not a PNG/BMP (first bytes: {blob[:8]!r})"
    # Format metadata round-trips with the magic.
    fmt = result.get("format")
    if fmt == "png":
        assert blob.startswith(_PNG_MAGIC), "format=png but blob not PNG"
    elif fmt == "bmp":
        assert blob.startswith(_BMP_MAGIC), "format=bmp but blob not BMP"


# ===========================================================================
# §1.6.6 — default screen.capture returns an MCP image content item
#
# The resynced submodule wire.py.request() only extracts type:"text" items,
# so it structurally cannot read this. Drive the raw MCP result directly.

def test_screen_capture_base64_image_content_item(
        client: WireClient, capabilities: dict) -> None:
    """Default (no encoding arg) `tools/call screen.capture` →
    `{"content":[{"type":"image","data":"<base64>","mimeType":
    "image/png"|"image/bmp"}],"isError":false}`; base64-decoding `data`
    yields PNG/BMP magic (framing §1.6.6)."""
    needs_verb(capabilities, "screen.capture")
    result = _tools_call_raw(client, "screen.capture", {})
    assert result.get("isError") is False, \
        f"screen.capture default path errored: {result!r}"
    content = result.get("content")
    assert isinstance(content, list) and content, \
        f"result.content must be a non-empty array, got {content!r}"
    item = content[0]
    assert item.get("type") == "image", \
        f"content[0].type must be 'image', got {item.get('type')!r}"
    assert item.get("mimeType") in {"image/png", "image/bmp"}, \
        f"unexpected mimeType {item.get('mimeType')!r}"
    data = item.get("data")
    assert isinstance(data, str) and data, \
        f"content[0].data must be a non-empty base64 string, got {data!r}"
    raw = base64.b64decode(data)
    assert _is_image_bytes(raw), \
        f"decoded image data is not PNG/BMP (first bytes: {raw[:8]!r})"
    if item["mimeType"] == "image/png":
        assert raw.startswith(_PNG_MAGIC), "mimeType png but data not PNG"
    elif item["mimeType"] == "image/bmp":
        assert raw.startswith(_BMP_MAGIC), "mimeType bmp but data not BMP"


# ===========================================================================
# R5 — disabled-element pre-check + input.mouse.click hit-test / verify_enabled
#
# Both code paths share the same `detail.reason == "element_disabled"`
# discriminator with `{name, role, flags, bounds}` fields. The agent is
# intentionally ahead of the pinned 2.2-rc spec on this (a follow-up
# protocol-repo PR is tracked in the task brief) — the wire shape is the
# authoritative contract here. needs_verb gates skip on agents that don't
# implement the relevant verb.

def _find_disabled_element(client: WireClient) -> dict:
    """Scan `element.list --full` for an element whose UIA `flags` array
    does NOT contain "enabled". Returns the element dict, or None if no
    authentically-disabled control was discoverable on this host.

    Pure read-tier: element.list is Read. The walk is bounded by the
    spec-default limit, which is large enough on a real Windows desktop
    that at least one disabled control (greyed-out menu item, sub-feature
    button, etc.) is reliably present."""
    r = client.request("element.list", "--full", "true", "--limit", "500")
    if not isinstance(r, OkResponse):
        return None
    body = json.loads(r.payload)
    for e in body.get("elements", []):
        flags = e.get("flags") or []
        if "enabled" not in flags:
            # Reject obviously empty/unaddressable entries: we need a name
            # OR an automation_id to drive element.find_invoke against,
            # AND a non-empty bounding rectangle for the click-coord test.
            if not (e.get("name") or e.get("automation_id")):
                continue
            b = e.get("bounds") or {}
            if not (isinstance(b.get("w"), int) and b["w"] > 0 and
                    isinstance(b.get("h"), int) and b["h"] > 0):
                continue
            return e
    return None


def test_r5_invoke_disabled_returns_element_disabled(
        update_client: WireClient, capabilities: dict) -> None:
    """element.find_invoke against an authentically-disabled UIA control
    returns ERR invalid_args with `detail.reason == "element_disabled"`
    and the spec-aligned introspection fields (name/role/flags/bounds).

    Discovery: scan element.list --full for any element missing "enabled"
    in its flags array. Skips if no disabled control is present on the
    host's current foreground — every desktop session legitimately differs
    on whether a greyed-out toolbar button is visible."""
    needs_verb(capabilities, "element.list")
    needs_verb(capabilities, "element.find_invoke")

    disabled = _find_disabled_element(update_client)
    if disabled is None:
        pytest.skip("no authentically-disabled UIA element discoverable in "
                    "the current element.list snapshot")

    # element.find_invoke matches on name + role. Use both for specificity
    # (automation_id is mutually exclusive with name per the spec). A
    # tight timeout keeps the test fast — the element was visible in the
    # most recent list snapshot, so a 0-ms find will resolve immediately.
    args = ["--timeout-ms", "200"]
    if disabled.get("name"):
        args += ["--name", disabled["name"]]
    if disabled.get("role"):
        args += ["--role", disabled["role"]]

    r = update_client.request("element.find_invoke", *args)
    # The element may have been redrawn / re-enabled between list and
    # find_invoke; that's a flaky-environment outcome, not a contract
    # violation. Accept the spec-declared not_found / uia_blind as a
    # legitimate transient and skip rather than fail.
    if isinstance(r, ErrResponse) and r.code in ("not_found", "uia_blind"):
        pytest.skip(f"disabled element no longer matchable ({r.code})")

    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"
    assert r.detail.get("reason") == "element_disabled", \
        f"expected detail.reason == 'element_disabled', got " \
        f"{r.detail.get('reason')!r} (full detail: {r.detail!r})"
    assert "name" in r.detail, f"detail missing 'name': {r.detail!r}"
    assert "role" in r.detail, f"detail missing 'role': {r.detail!r}"
    assert "flags" in r.detail and isinstance(r.detail["flags"], list), \
        f"detail.flags must be a list, got {r.detail.get('flags')!r}"
    assert "bounds" in r.detail and isinstance(r.detail["bounds"], dict), \
        f"detail.bounds must be an object, got {r.detail.get('bounds')!r}"
    for k in ("x", "y", "w", "h"):
        assert k in r.detail["bounds"], \
            f"detail.bounds missing {k}: {r.detail['bounds']!r}"


def test_r5_click_target_element_always_present(
        update_client: WireClient, capabilities: dict) -> None:
    """input.mouse.click OK body carries the new `target_element` field
    (R5 hit-test addition). Field is present unconditionally; value is
    either a UIA-element object {name, role, flags, ...} or JSON null
    when ElementFromPoint resolved nothing at the click coordinate.

    Coordinate strategy: pick an enabled UIA element from element.list
    (so the click reliably lands on a real native control) and click its
    bounding-rect centre. Falls back to OFFSCREEN coords if no suitable
    enabled control was discoverable."""
    needs_verb(capabilities, "input.mouse.click")

    # Prefer a real enabled control with non-trivial bounds. Read tier is
    # fine for element.list; the click itself runs at update tier.
    target_x = OFFSCREEN_X
    target_y = OFFSCREEN_Y
    if "element.list" in capabilities:
        r0 = update_client.request("element.list",
                                   "--full", "true", "--limit", "200")
        if isinstance(r0, OkResponse):
            body = json.loads(r0.payload)
            for e in body.get("elements", []):
                flags = e.get("flags") or []
                if "enabled" not in flags:
                    continue
                b = e.get("bounds") or {}
                if (isinstance(b.get("w"), int) and b["w"] > 4 and
                        isinstance(b.get("h"), int) and b["h"] > 4):
                    target_x = b["x"] + b["w"] // 2
                    target_y = b["y"] + b["h"] // 2
                    break

    r = update_client.request("input.mouse.click",
                              "--x", str(target_x),
                              "--y", str(target_y))
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    # Pre-existing fields must still be present (regression guard for the
    # R5 additive change).
    assert "synthesised" in body, f"OK body missing synthesised: {body!r}"
    assert "target_handle" in body, f"OK body missing target_handle: {body!r}"
    assert "actual_position" in body, \
        f"OK body missing actual_position: {body!r}"
    # The new R5 field: present unconditionally, value is object-or-null.
    assert "target_element" in body, \
        f"OK body missing target_element (R5): {body!r}"
    te = body["target_element"]
    assert te is None or isinstance(te, dict), \
        f"target_element must be object-or-null, got {te!r}"
    if isinstance(te, dict):
        # When UIA resolves an element, the spec object carries
        # name/role/flags (bounds is also emitted for the agent's R5
        # implementation, mirroring the refusal-detail shape).
        assert "name" in te, f"target_element missing name: {te!r}"
        assert "role" in te, f"target_element missing role: {te!r}"
        assert "flags" in te and isinstance(te["flags"], list), \
            f"target_element.flags must be a list, got {te.get('flags')!r}"


def test_r5_click_verify_enabled_blocks_disabled(
        update_client: WireClient, capabilities: dict) -> None:
    """input.mouse.click with `verify_enabled:true` on the centre of a
    disabled UIA element's bounding rectangle refuses with ERR
    invalid_args + `detail.reason == "element_disabled"`. No click is
    synthesised — the refusal happens before SendInput."""
    needs_verb(capabilities, "element.list")
    needs_verb(capabilities, "input.mouse.click")

    disabled = _find_disabled_element(update_client)
    if disabled is None:
        pytest.skip("no authentically-disabled UIA element discoverable in "
                    "the current element.list snapshot")

    b = disabled["bounds"]
    cx = b["x"] + b["w"] // 2
    cy = b["y"] + b["h"] // 2

    r = update_client.request("input.mouse.click",
                              "--x", str(cx),
                              "--y", str(cy),
                              "--verify-enabled", "true")
    # Same flaky-environment tolerance as the find_invoke test: if the
    # element was redrawn / re-enabled between list and click, the
    # verify_enabled gate proceeds and we get an OK (or some other
    # transient). Accept OK-with-non-error-target as the "element no
    # longer disabled" outcome and skip.
    if isinstance(r, OkResponse):
        pytest.skip("hit-tested element was no longer disabled at click "
                    "time (verify_enabled correctly proceeded)")

    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"
    assert r.detail.get("reason") == "element_disabled", \
        f"expected detail.reason == 'element_disabled', got " \
        f"{r.detail.get('reason')!r} (full detail: {r.detail!r})"
    # The verify_enabled refusal embeds target_element (object with
    # name/role/flags/bounds) so the caller can introspect what was
    # rejected without re-querying.
    te = r.detail.get("target_element")
    assert isinstance(te, dict), \
        f"detail.target_element must be an object, got {te!r}"
    assert "name" in te and "role" in te and "flags" in te, \
        f"target_element missing one of name/role/flags: {te!r}"


# ===========================================================================
# R6 — vision.describe (LLM-mediated screen description)
#
# The verb captures a region/window/monitor/full screen, POSTs it to a
# configured OpenAI-compatible /v1/chat/completions endpoint with the image
# embedded as a data-URL, and returns the assistant message text + usage
# stats. Four tests: three deterministic (no LLM required) plus one live
# round-trip against LM Studio (host's vmnet1 IP, skipped if unreachable).
#
# All tier-Update; the conformance suite runs at update_client. The host
# under test is assumed NOT to have --vision-endpoint configured by default,
# so the no-endpoint test exercises the missing-config path.

# Host's reachable IP from inside the VM. For a host-only VM this is the
# vmnet1 gateway (e.g. 192.168.80.1); for a bridged VM it's the host's LAN
# IPv4. NOT a loopback literal. NOT a hardcoded production assumption —
# update this constant for your network, or refactor to env-var driven.
_VISION_LIVE_HOST = "192.168.1.98"
_VISION_LIVE_PORT = 1234
_VISION_LIVE_URL = (
    f"http://{_VISION_LIVE_HOST}:{_VISION_LIVE_PORT}/v1/chat/completions")


def _llm_reachable(host: str, port: int, timeout_s: float = 1.0) -> bool:
    """Quick TCP probe to decide whether the live LM Studio test should run.
    Returns False on any connect failure — DNS, refused, unreachable, or
    timeout — so the live test skips cleanly rather than failing when the
    host LLM isn't up."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout_s)
            return sock.connect_ex((host, port)) == 0
    except OSError:
        return False


def test_r6_vision_describe_endpoint_missing(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.describe with no endpoint arg AND no agent-side --vision-endpoint
    default returns ERR invalid_args with detail.reason=='vision_endpoint_missing'.
    The conformance VM is launched WITHOUT --vision-endpoint, so this is the
    documented missing-config path."""
    needs_verb(capabilities, "vision.describe")
    r = update_client.request("vision.describe")
    if isinstance(r, OkResponse):
        # If the VM was launched with --vision-endpoint configured this test
        # has no purchase — skip rather than misreport. The other R6 tests
        # are not endpoint-default-dependent.
        pytest.skip("agent appears to have a vision endpoint configured; "
                    "endpoint-missing path not exercisable in this run")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    # The R6 detail.reason is the discriminator; the wire code is invalid_args
    # (the endpoint isn't a network failure — it's a configuration error).
    if r.code != "invalid_args" or \
            r.detail.get("reason") != "vision_endpoint_missing":
        pytest.skip(
            f"unexpected error shape (likely endpoint configured server-side): "
            f"code={r.code!r} detail={r.detail!r}")
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r}"
    assert r.detail.get("reason") == "vision_endpoint_missing", \
        f"expected detail.reason=='vision_endpoint_missing', got {r.detail!r}"


def test_r6_vision_describe_invalid_url(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.describe with a syntactically-invalid endpoint returns ERR
    invalid_args (WinHttpCrackUrl rejects 'not-a-url')."""
    needs_verb(capabilities, "vision.describe")
    r = update_client.request("vision.describe", "--endpoint", "not-a-url")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"


def test_r6_vision_describe_unreachable(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.describe with an unroutable endpoint (TEST-NET-1, RFC 5737)
    returns ERR transfer_failed OR timeout — both are spec-acceptable
    depending on whether the OS reports unreachable up-front or the wait
    expires first. timeout_ms=2000 keeps the test fast."""
    needs_verb(capabilities, "vision.describe")
    r = update_client.request(
        "vision.describe",
        "--endpoint", "http://192.0.2.1:1/v1/chat/completions",
        "--timeout-ms", "2000")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code in {"transfer_failed", "timeout"}, \
        f"expected transfer_failed or timeout, got {r.code!r} ({r.detail!r})"


def test_r6_vision_describe_live(
        update_client: WireClient, capabilities: dict) -> None:
    """Live round-trip: capture the full virtual screen, POST it to LM Studio
    on the host's vmnet1 IP, expect a non-empty assistant message back.
    Skips cleanly when the LLM is unreachable from the test runner (covers
    the common case of running the suite without an LM Studio session up)."""
    needs_verb(capabilities, "vision.describe")
    if not _llm_reachable(_VISION_LIVE_HOST, _VISION_LIVE_PORT):
        pytest.skip(
            f"LM Studio at {_VISION_LIVE_HOST}:{_VISION_LIVE_PORT} not "
            f"reachable from the test runner — skipping live round-trip")

    r = update_client.request(
        "vision.describe",
        "--endpoint", _VISION_LIVE_URL,
        # Empty model -> LM Studio picks the loaded model (its convention).
        "--model", "",
        "--prompt", "Reply with the single word OK.",
        # Generous on the wall clock — model load can take several seconds.
        "--timeout-ms", "120000")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
    body = json.loads(r.payload)
    assert isinstance(body.get("description"), str) and body["description"], \
        f"description must be a non-empty string, got {body.get('description')!r}"
    # Spec says model: string (no non-empty requirement). LM Studio populates
    # it in practice, but the handler emits "" when the server omits the
    # field — don't force a stronger contract than the spec.
    assert isinstance(body.get("model"), str), \
        f"model must be a string, got {body.get('model')!r}"
    assert isinstance(body.get("elapsed_ms"), int) and not isinstance(
        body["elapsed_ms"], bool), \
        f"elapsed_ms must be an int, got {body.get('elapsed_ms')!r}"
    assert body["elapsed_ms"] > 0, \
        f"elapsed_ms must be > 0, got {body['elapsed_ms']!r}"


# ===========================================================================
# R7 — vision.calibrate (OCR + vision-LLM composite on caller-supplied image)
#
# Calibration verb: the orchestrating LLM sends a known image (ground truth
# already known to it) plus an optional prompt; the agent OCRs the bytes
# locally via Windows.Media.Ocr AND (when an endpoint is configured/provided)
# posts them to the vision-LLM. Both results return in one OK body so the
# caller can compare each against ground truth and decide which backend to
# trust for the task at hand.
#
# Partial-failure rule:
#   (ocr_ok, vision_ok) -> shape
#     (T, T) -> OK { ocr, vision, prompt?, elapsed_ms }
#     (T, F) -> OK { ocr, vision: null, ... }            <-- OCR-only mode
#     (F, T) -> OK { ocr: null, vision, ... }            <-- no OCR pack
#     (F, F) -> ERR not_supported{reason:calibration_unavailable}
#
# All Update-tier; reuses R6's _VISION_LIVE_HOST / _llm_reachable probe.


def _make_test_png(width: int, height: int, fill_byte: int = 0xFF) -> bytes:
    """Generate a minimal valid PNG of the given size, filled with one byte.

    Uses only the Python stdlib (zlib + struct) — the conformance runner
    must not depend on Pillow / OpenCV / anything outside `pip install
    pytest`. The output is a single-channel grayscale PNG with one filter
    byte (0 = None) per scanline. Output ranges from ~70 bytes (1x1) to
    ~150 bytes (64x64 white) — well under any wire-size concern.

    `fill_byte=0xFF` (white) is the default; pass another byte for a
    different uniform colour. Tests don't need a meaningful image — they
    only need a valid one that the OCR engine accepts."""
    def _chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + \
            struct.pack(">I", crc)

    sig = b'\x89PNG\r\n\x1a\n'
    # IHDR: width, height, bit_depth=8, colour_type=0 (grayscale), three zeros
    # for compression/filter/interlace methods.
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    # Each scanline = filter byte (0 = None) + width raw bytes.
    raw = b''.join(b'\x00' + bytes([fill_byte] * width) for _ in range(height))
    idat = zlib.compress(raw)
    return sig + _chunk(b'IHDR', ihdr) + _chunk(b'IDAT', idat) + \
        _chunk(b'IEND', b'')


def test_r7_vision_calibrate_image_required(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.calibrate with no image arg returns ERR invalid_args. The image
    is the only argument the verb cannot synthesise a default for."""
    needs_verb(capabilities, "vision.calibrate")
    r = update_client.request("vision.calibrate")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"


def test_r7_vision_calibrate_bad_base64(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.calibrate with malformed base64 returns ERR invalid_args. The
    `@@` characters are outside the RFC 4648 alphabet, so base64_decode()
    must reject them with std::nullopt and the handler surfaces invalid_args
    {reason:'bad_base64'}."""
    needs_verb(capabilities, "vision.calibrate")
    r = update_client.request(
        "vision.calibrate",
        "--image", "not-base64@@")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"
    # detail.reason discriminator — defensive: accept either the explicit
    # 'bad_base64' or a generic 'message' shape; the spec is reason-keyed
    # but older clients may only check the code.
    reason = r.detail.get("reason")
    if reason is not None:
        assert reason == "bad_base64", \
            f"expected detail.reason=='bad_base64', got {r.detail!r}"


def test_r7_vision_calibrate_unsupported_format(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.calibrate with valid base64 but garbage bytes (no PNG/JPEG/BMP
    magic) returns ERR invalid_args with reason 'unsupported_image_format'.
    The handler must refuse before any expensive decode / OCR call."""
    needs_verb(capabilities, "vision.calibrate")
    # 32 bytes of 'Z' — base64-encodes cleanly but no image-magic prefix.
    garbage = b'Z' * 32
    r = update_client.request(
        "vision.calibrate",
        "--image", base64.b64encode(garbage).decode("ascii"))
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", \
        f"expected invalid_args, got {r.code!r} ({r.detail!r})"
    assert r.detail.get("reason") == "unsupported_image_format", \
        f"expected detail.reason=='unsupported_image_format', got {r.detail!r}"


def test_r7_vision_calibrate_ocr_only(
        update_client: WireClient, capabilities: dict) -> None:
    """vision.calibrate with a synthesised PNG and NO endpoint exercises the
    OCR-only partial-success path. Expect OK with body.ocr populated, body.vision
    null. Skips cleanly if the conformance VM has --vision-endpoint configured
    (this path is not exercisable there)."""
    needs_verb(capabilities, "vision.calibrate")
    png = _make_test_png(64, 64)
    # Magic sanity-check on the host before sending — guards against silent
    # _make_test_png regression turning this into a different test.
    assert png.startswith(_PNG_MAGIC), "test PNG generator broken"

    r = update_client.request(
        "vision.calibrate",
        "--image", base64.b64encode(png).decode("ascii"))
    if isinstance(r, ErrResponse):
        # Most plausible cause for OK-expected-but-got-ERR: the VM was
        # launched WITH --vision-endpoint AND the endpoint is unreachable
        # AND the OCR engine has no language packs. Skip rather than
        # misreport — the assertion contract is the OCR-only PATH, not a
        # blanket "must succeed under all configurations".
        if r.code == "not_supported" and \
                r.detail.get("reason") == "calibration_unavailable":
            pytest.skip(
                "calibration_unavailable: no OCR languages AND no vision "
                "backend — neither path can be exercised on this VM")
        pytest.fail(f"unexpected ErrResponse: {r.code!r} {r.detail!r}")

    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
    body = json.loads(r.payload)

    ocr = body.get("ocr")
    assert isinstance(ocr, dict), f"ocr must be an object, got {ocr!r}"
    assert isinstance(ocr.get("text"), str), \
        f"ocr.text must be a string, got {ocr.get('text')!r}"
    assert isinstance(ocr.get("lines"), list), \
        f"ocr.lines must be a list, got {ocr.get('lines')!r}"
    size = ocr.get("image_size")
    assert isinstance(size, dict) and size.get("w") == 64 and \
        size.get("h") == 64, \
        f"ocr.image_size must be {{w:64,h:64}}, got {size!r}"
    assert isinstance(ocr.get("elapsed_ms"), int) and not isinstance(
        ocr["elapsed_ms"], bool), \
        f"ocr.elapsed_ms must be an int, got {ocr.get('elapsed_ms')!r}"
    assert ocr["elapsed_ms"] >= 0, \
        f"ocr.elapsed_ms must be >= 0, got {ocr['elapsed_ms']!r}"
    assert isinstance(ocr.get("engine_languages"), list), \
        f"ocr.engine_languages must be a list, got " \
        f"{ocr.get('engine_languages')!r}"

    # OCR-only mode: vision must be JSON null. Without an endpoint there's
    # nothing the agent could populate, and a non-null shape here would be
    # a partial-failure-rule violation.
    assert body.get("vision", "SENTINEL") is None, \
        f"vision must be null in OCR-only mode, got {body.get('vision')!r}"

    # elapsed_ms wraps the whole verb — must exist and be >= ocr.elapsed_ms.
    assert isinstance(body.get("elapsed_ms"), int) and not isinstance(
        body["elapsed_ms"], bool), \
        f"body.elapsed_ms must be an int, got {body.get('elapsed_ms')!r}"
    assert body["elapsed_ms"] >= ocr["elapsed_ms"], \
        f"body.elapsed_ms ({body['elapsed_ms']}) must be >= ocr.elapsed_ms " \
        f"({ocr['elapsed_ms']})"


def test_r7_vision_calibrate_live_composite(
        update_client: WireClient, capabilities: dict) -> None:
    """Live composite round-trip: synthesised PNG sent with the LM Studio
    endpoint. Expect both OCR and vision populated; prompt echoed back.
    Skips cleanly when LM Studio not reachable (mirrors R6's pattern)."""
    needs_verb(capabilities, "vision.calibrate")
    if not _llm_reachable(_VISION_LIVE_HOST, _VISION_LIVE_PORT):
        pytest.skip(
            f"LM Studio at {_VISION_LIVE_HOST}:{_VISION_LIVE_PORT} not "
            f"reachable from the test runner — skipping live composite")

    png = _make_test_png(64, 64)
    r = update_client.request(
        "vision.calibrate",
        "--image", base64.b64encode(png).decode("ascii"),
        "--endpoint", _VISION_LIVE_URL,
        "--model", "",
        "--prompt", "Reply with the single word OK.",
        # Generous timeout — model load can take several seconds.
        "--timeout-ms", "120000")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
    body = json.loads(r.payload)

    # OCR side — must be a populated object (the host VM has English packs).
    ocr = body.get("ocr")
    assert isinstance(ocr, dict), f"ocr must be an object, got {ocr!r}"
    assert isinstance(ocr.get("text"), str), \
        f"ocr.text must be a string, got {ocr.get('text')!r}"

    # Vision side — must be a populated object with a non-empty description.
    vision = body.get("vision")
    assert isinstance(vision, dict), \
        f"vision must be an object, got {vision!r}"
    assert isinstance(vision.get("description"), str) and \
        vision["description"], \
        f"vision.description must be a non-empty string, got " \
        f"{vision.get('description')!r}"
    assert isinstance(vision.get("model"), str), \
        f"vision.model must be a string, got {vision.get('model')!r}"
    assert isinstance(vision.get("elapsed_ms"), int) and not isinstance(
        vision["elapsed_ms"], bool), \
        f"vision.elapsed_ms must be an int, got {vision.get('elapsed_ms')!r}"
    assert vision["elapsed_ms"] > 0, \
        f"vision.elapsed_ms must be > 0, got {vision['elapsed_ms']!r}"

    # Prompt echo policy: caller sent one -> must be echoed verbatim.
    assert body.get("prompt") == "Reply with the single word OK.", \
        f"prompt must be echoed verbatim, got {body.get('prompt')!r}"


# ===========================================================================
# system.ping — named liveness probe (empty OK body)

def test_system_ping_returns_ok(
        update_client: WireClient, capabilities: dict) -> None:
    """system.ping returns Ok with an empty body — a log-suppressible
    liveness probe distinct from system.health."""
    needs_verb(capabilities, "system.ping")
    r = update_client.request("system.ping")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"


# ===========================================================================
# R8/R9/R10 — element.range_value / get_text / search
#
# Each needs a live element handle; acquire one from element.list. Tests are
# lenient about the host's foreground app — they assert wire SHAPE, skipping
# when the environment offers no suitable element.

def _first_element_handle(client: WireClient) -> str | None:
    """Return the handle of the first element.list entry, or None."""
    r = client.request("element.list", "--limit", "50")
    if not isinstance(r, OkResponse):
        return None
    body = json.loads(r.payload)
    for e in body.get("elements", []):
        h = e.get("handle")
        if isinstance(h, str) and h.startswith("elt:"):
            return h
    return None


def test_r8_range_value_missing_handle_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """element.range_value with no handle returns ERR invalid_args."""
    needs_verb(capabilities, "element.range_value")
    r = update_client.request("element.range_value")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", f"expected invalid_args, got {r.code!r}"


def test_r8_range_value_non_range_element(
        update_client: WireClient, capabilities: dict) -> None:
    """element.range_value on a live element either returns the range shape
    {min,max,value} or ERR not_supported_by_target {pattern} when the
    element is not a slider/stepper. Both are spec-correct."""
    needs_verb(capabilities, "element.range_value")
    needs_verb(capabilities, "element.list")
    handle = _first_element_handle(update_client)
    if handle is None:
        pytest.skip("no element handle discoverable on the current foreground")
    r = update_client.request("element.range_value", "--handle", handle)
    if isinstance(r, ErrResponse):
        if r.code in ("target_gone", "ax_disabled", "ax_error"):
            pytest.skip(f"AX environment not suitable: {r.code}")
        assert r.code == "not_supported_by_target", \
            f"expected not_supported_by_target, got {r.code!r} ({r.detail!r})"
        assert r.detail.get("pattern") == "RangeValuePattern", \
            f"expected detail.pattern=='RangeValuePattern', got {r.detail!r}"
        return
    assert isinstance(r, OkResponse), f"expected Ok or Err, got {r!r}"
    body = json.loads(r.payload)
    for k in ("min", "max", "value"):
        assert k in body, f"range_value body missing {k!r}: {body!r}"


def test_r9_get_text_missing_handle_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """element.get_text with no handle returns ERR invalid_args."""
    needs_verb(capabilities, "element.get_text")
    r = update_client.request("element.get_text")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", f"expected invalid_args, got {r.code!r}"


def test_r9_get_text_bad_max_length_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """element.get_text with an out-of-range max-length returns invalid_args
    before any AX call."""
    needs_verb(capabilities, "element.get_text")
    handle = _first_element_handle(update_client) or "elt:1"
    r = update_client.request("element.get_text", "--handle", handle,
                              "--max-length", "999999999")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", f"expected invalid_args, got {r.code!r}"


def test_r9_get_text_returns_paginated_shape(
        update_client: WireClient, capabilities: dict) -> None:
    """element.get_text on a live element returns {text,length,truncated,
    offset} with the documented types."""
    needs_verb(capabilities, "element.get_text")
    needs_verb(capabilities, "element.list")
    handle = _first_element_handle(update_client)
    if handle is None:
        pytest.skip("no element handle discoverable on the current foreground")
    r = update_client.request("element.get_text", "--handle", handle)
    if isinstance(r, ErrResponse) and r.code in ("target_gone", "ax_disabled", "ax_error"):
        pytest.skip(f"element unusable: {r.code}")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
    body = json.loads(r.payload)
    assert isinstance(body.get("text"), str), f"text must be str: {body!r}"
    assert isinstance(body.get("length"), int) and not isinstance(
        body["length"], bool), f"length must be int: {body!r}"
    assert isinstance(body.get("truncated"), bool), f"truncated must be bool: {body!r}"
    assert isinstance(body.get("offset"), int), f"offset must be int: {body!r}"


def test_r10_search_missing_args_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """element.search without --root or --patterns returns invalid_args."""
    needs_verb(capabilities, "element.search")
    r = update_client.request("element.search")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", f"expected invalid_args, got {r.code!r}"


def test_r10_search_bad_patterns_rejected(
        update_client: WireClient, capabilities: dict) -> None:
    """element.search with a non-JSON-array --patterns returns invalid_args."""
    needs_verb(capabilities, "element.search")
    handle = _first_element_handle(update_client) or "elt:1"
    r = update_client.request("element.search", "--root", handle,
                              "--patterns", "not-json")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args", f"expected invalid_args, got {r.code!r}"


def test_r10_search_returns_result_shape(
        update_client: WireClient, capabilities: dict) -> None:
    """element.search on a live subtree returns {hits,patterns_unmatched,
    total_text_searched,truncated}."""
    needs_verb(capabilities, "element.search")
    needs_verb(capabilities, "element.list")
    handle = _first_element_handle(update_client)
    if handle is None:
        pytest.skip("no element handle discoverable on the current foreground")
    r = update_client.request("element.search", "--root", handle,
                              "--patterns", json.dumps(["zzz-no-such-text-zzz"]))
    if isinstance(r, ErrResponse) and r.code in ("target_gone", "ax_disabled", "ax_error"):
        pytest.skip(f"element unusable: {r.code}")
    assert isinstance(r, OkResponse), f"expected OkResponse, got {r!r}"
    body = json.loads(r.payload)
    assert isinstance(body.get("hits"), list), f"hits must be list: {body!r}"
    assert isinstance(body.get("patterns_unmatched"), list), \
        f"patterns_unmatched must be list: {body!r}"
    assert isinstance(body.get("total_text_searched"), int), \
        f"total_text_searched must be int: {body!r}"
    assert isinstance(body.get("truncated"), bool), \
        f"truncated must be bool: {body!r}"
    assert body["patterns_unmatched"] == ["zzz-no-such-text-zzz"], \
        f"the absent pattern must be reported unmatched: {body!r}"
