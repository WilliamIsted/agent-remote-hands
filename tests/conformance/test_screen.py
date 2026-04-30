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

"""Tests for `screen.*`."""

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


def test_screen_capture_full(client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture")
    assert isinstance(r, OkResponse)
    assert len(r.payload) > 0


def test_screen_capture_png_signature(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--format", "png")
    assert isinstance(r, OkResponse)
    # PNG magic: 89 50 4E 47 0D 0A 1A 0A
    assert r.payload[:8] == b"\x89PNG\r\n\x1a\n", \
        f"expected PNG header, got {r.payload[:8]!r}"


def test_screen_capture_bmp_signature(client: WireClient,
                                      capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--format", "bmp")
    assert isinstance(r, OkResponse)
    assert r.payload[:2] == b"BM", \
        f"expected BMP header, got {r.payload[:2]!r}"


def test_screen_capture_region(client: WireClient,
                               capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--region", "0,0,100,100")
    assert isinstance(r, OkResponse)
    assert len(r.payload) > 0


def test_screen_capture_invalid_format(client: WireClient,
                                       capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--format", "tiff-2000")
    assert isinstance(r, ErrResponse)
    assert r.code == "not_supported"


def test_screen_capture_region_and_window_mutually_exclusive(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture",
                       "--region", "0,0,10,10",
                       "--window", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_screen_capture_no_cursor_flag_accepted(client: WireClient,
                                                capabilities: dict) -> None:
    """`--no-cursor` is the documented opt-out from cursor compositing.
    Must succeed (cursor in screenshot is the default but this flag
    suppresses the overlay)."""
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--region", "0,0,100,100",
                       "--no-cursor")
    assert isinstance(r, OkResponse)
    assert len(r.payload) > 0


def test_screen_capture_unknown_flag_rejected(client: WireClient,
                                              capabilities: dict) -> None:
    """The verb parser must reject unknown flags rather than silently
    accepting them. Previously `--cursor` (typo for `--no-cursor`) would
    return OK with no effect — a real footgun."""
    needs_verb(capabilities, "screen.capture")
    r = client.request("screen.capture", "--bogus-flag")
    assert isinstance(r, ErrResponse), f"expected ErrResponse, got {r!r}"
    assert r.code == "invalid_args"
    assert r.detail.get("unknown_flag") == "--bogus-flag"
