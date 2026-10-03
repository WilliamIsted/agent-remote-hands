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

"""Tests for `vision.*`.

Live capture sources (region/window/monitor) skip-clean on agents that don't
advertise vision.ocr. The path-source test composes screen.capture +
file.create + vision.ocr + file.delete to avoid re-introducing a fixtures/
directory."""

import json
import pathlib
import tempfile
import uuid

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# Live source — region

def test_vision_ocr_region_returns_required_shape(client: WireClient,
                                                  capabilities: dict) -> None:
    """OCR a small region of the desktop. Result is non-empty on most systems
    (taskbar text, window titles) but tolerate empty. Asserts the full v2.1
    response shape with `coordinate_space`, `image_size`, `text_angle`."""
    needs_verb(capabilities, "vision.ocr")
    r = client.request("vision.ocr", "--region", "0,0,800,200")
    assert isinstance(r, OkResponse), f"got {r!r}"
    body = json.loads(r.payload)
    for k in ("text", "lines", "language_used",
              "coordinate_space", "image_size", "text_angle"):
        assert k in body, f"vision.ocr response missing {k}: {body}"
    assert body["coordinate_space"] == "screen"
    assert isinstance(body["lines"], list)
    for line in body["lines"]:
        assert "text" in line
        assert "bbox" in line
        for k in ("x", "y", "w", "h"):
            assert k in line["bbox"], f"line bbox missing {k}: {line}"


# ---------------------------------------------------------------------------
# Selector validation

def test_vision_ocr_mutually_exclusive_selectors(client: WireClient,
                                                 capabilities: dict) -> None:
    """region + window passed together must reject as invalid_args per the
    x-mutually-exclusive constraint."""
    needs_verb(capabilities, "vision.ocr")
    r = client.request("vision.ocr",
                       "--region", "0,0,100,100",
                       "--window", "win:0x1")
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


def test_vision_ocr_invalid_window_returns_not_found(
        client: WireClient, capabilities: dict) -> None:
    needs_verb(capabilities, "vision.ocr")
    r = client.request("vision.ocr", "--window", "win:0xFFFFFFFF")
    assert isinstance(r, ErrResponse)
    assert r.code in ("not_found", "invalid_args")


# ---------------------------------------------------------------------------
# Language handling

def test_vision_ocr_unsupported_language(client: WireClient,
                                         capabilities: dict) -> None:
    """An exotic BCP-47 tag that won't be installed exercises the
    not_supported path with `available_languages` detail. Tolerate
    invalid_args for agents that pre-validate the tag format."""
    needs_verb(capabilities, "vision.ocr")
    r = client.request("vision.ocr",
                       "--region", "0,0,100,100",
                       "--language", "xx-INVALID")
    if isinstance(r, ErrResponse):
        assert r.code in ("not_supported", "invalid_args")


# ---------------------------------------------------------------------------
# bytes input — x-conditional enforcement

def test_vision_ocr_bytes_without_format_rejected(
        client: WireClient, capabilities: dict) -> None:
    """bytes_format is required when bytes is supplied (x-conditional rule).
    Sending bytes payload alone must reject as invalid_args."""
    needs_verb(capabilities, "vision.ocr")
    payload = b"\x89PNG\r\n\x1a\n"  # PNG magic; content irrelevant
    r = client.request("vision.ocr", str(len(payload)), payload=payload)
    assert isinstance(r, ErrResponse)
    assert r.code == "invalid_args"


# ---------------------------------------------------------------------------
# path source — composite screen.capture → file.create → vision.ocr → file.delete

def test_vision_ocr_path_source(delete_client: WireClient,
                                capabilities: dict) -> None:
    """Capture a small region as PNG, write it to a temp path, OCR via
    --path, clean up. Verifies the static-source code path without
    re-introducing a checked-in fixtures/ directory."""
    needs_verb(capabilities, "vision.ocr")
    needs_verb(capabilities, "screen.capture")
    needs_verb(capabilities, "file.create")
    needs_verb(capabilities, "file.delete")

    r = delete_client.request("screen.capture",
                              "--region", "0,0,400,100",
                              "--format", "png")
    assert isinstance(r, OkResponse), f"screen.capture failed: {r!r}"
    png_bytes = r.payload

    path = str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-vision-test-{uuid.uuid4().hex}.png")
    r = delete_client.request("file.create", path,
                              str(len(png_bytes)), payload=png_bytes)
    assert isinstance(r, OkResponse), f"file.create failed: {r!r}"

    try:
        r = delete_client.request("vision.ocr", "--path", path)
        assert isinstance(r, OkResponse), f"vision.ocr failed: {r!r}"
        body = json.loads(r.payload)
        assert body["coordinate_space"] == "image"
        assert body["image_size"]["w"] > 0
        assert body["image_size"]["h"] > 0
    finally:
        delete_client.request("file.delete", path)
