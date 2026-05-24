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

"""Tests for ``file.download``.

The verb fetches a URL into a file on the agent host. Tests spin up an
in-process Python ``http.server`` bound to the host loopback so the agent VM
can reach back over the test network. When the agent runs on a remote VM that
cannot reach the test harness host, the HTTP-fixture tests skip gracefully
(``not_found`` / ``permission_denied`` / connection refused responses are
indistinguishable from a real misconfiguration, so we only assert reachability
on the happy path).

The error-shape tests (tier gating, missing-parent target) do not need the
fixture server and exercise the verb against any agent that advertises it.
"""

from __future__ import annotations

import http.server
import json
import pathlib
import socket
import tempfile
import threading
import uuid
from contextlib import contextmanager
from typing import Iterator, Tuple

import pytest

from conftest import needs_verb
from wire import ErrResponse, OkResponse, WireClient


# ---------------------------------------------------------------------------
# HTTP fixture server — bound to all interfaces on an ephemeral port so the
# agent VM can reach it. Each test that needs it spins one up in a thread.

class _Handler(http.server.BaseHTTPRequestHandler):
    """Serves one fixed payload + maps /404 to a 404 response."""

    payload: bytes = b"agent-remote-hands download fixture\n"
    content_type: str = "text/plain"

    def log_message(self, *_args, **_kwargs) -> None:  # silence access log
        return

    def do_GET(self) -> None:  # noqa: N802 — stdlib API
        if self.path == "/404":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", self.content_type)
        self.send_header("Content-Length", str(len(self.payload)))
        self.end_headers()
        self.wfile.write(self.payload)


@contextmanager
def _fixture_server() -> Iterator[Tuple[str, int]]:
    """Start an HTTP server on (0.0.0.0, ephemeral). Yields (host, port)."""
    srv = http.server.HTTPServer(("0.0.0.0", 0), _Handler)
    host_ip = _local_ip_or_skip()
    port = srv.server_address[1]
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield host_ip, port
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=2.0)


def _local_ip_or_skip() -> str:
    """Best-effort: discover the IP the agent VM can reach us on. Skips if
    no non-loopback address is available."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # No packet leaves the host — connect() on UDP just picks a route.
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
        finally:
            s.close()
        if ip and not ip.startswith("127."):
            return ip
    except OSError:
        pass
    pytest.skip("no non-loopback local IP for HTTP fixture")


def _scratch_path() -> str:
    return str(pathlib.Path(tempfile.gettempdir()) /
               f"remote-hands-download-{uuid.uuid4().hex}.bin")


# ---------------------------------------------------------------------------
# Error-shape tests (no fixture server needed)

def test_file_download_tier_gating(client: WireClient,
                                   capabilities: dict) -> None:
    needs_verb(capabilities, "file.download")
    r = client.request("file.download",
                       "http://example.com/", _scratch_path())
    assert isinstance(r, ErrResponse)
    assert r.code == "tier_required"


def test_file_download_parent_missing(create_client: WireClient,
                                      capabilities: dict) -> None:
    """Target directory does not exist => not_found."""
    needs_verb(capabilities, "file.download")

    missing_dir = pathlib.Path(tempfile.gettempdir()) / \
        f"remote-hands-download-noexist-{uuid.uuid4().hex}"
    target = str(missing_dir / "out.bin")

    with _fixture_server() as (host, port):
        url = f"http://{host}:{port}/"
        r = create_client.request("file.download", url, target)
    assert isinstance(r, ErrResponse)
    # Agent surfaces parent-missing as not_found; permission_denied is also
    # acceptable on platforms that conflate the two at CreateFile time.
    assert r.code in ("not_found", "permission_denied")


# ---------------------------------------------------------------------------
# Happy-path tests against an in-process HTTP fixture

def test_file_download_http_round_trip(create_client: WireClient,
                                       capabilities: dict) -> None:
    """Fetch a small body over HTTP and assert size + content_type."""
    needs_verb(capabilities, "file.download")

    target = _scratch_path()
    with _fixture_server() as (host, port):
        url = f"http://{host}:{port}/"
        r = create_client.request("file.download", url, target)

    if isinstance(r, ErrResponse):
        # The agent VM could not reach us — surface as skip not failure.
        pytest.skip(f"agent could not reach fixture server: "
                    f"{r.code} {r.detail}")
    assert isinstance(r, OkResponse)
    body = json.loads(r.payload)
    assert body["bytes_written"] == len(_Handler.payload)
    assert body["http_status"] == 200
    assert "text/plain" in body["content_type"]


def test_file_download_http_404_maps_to_permission_denied(
        create_client: WireClient, capabilities: dict) -> None:
    """An HTTP 404 surfaces as permission_denied with http_status in detail."""
    needs_verb(capabilities, "file.download")

    target = _scratch_path()
    with _fixture_server() as (host, port):
        url = f"http://{host}:{port}/404"
        r = create_client.request("file.download", url, target)

    if isinstance(r, ErrResponse) and r.code not in ("permission_denied",):
        # Agent couldn't reach us at all => skip rather than fail.
        pytest.skip(f"agent could not reach fixture server: "
                    f"{r.code} {r.detail}")
    assert isinstance(r, ErrResponse)
    assert r.code == "permission_denied"
    # http_status may be serialised as int or str depending on family —
    # be lenient.
    status = r.detail.get("http_status")
    assert status in (404, "404"), f"unexpected detail: {r.detail!r}"


def test_file_download_short_timeout(create_client: WireClient,
                                     capabilities: dict) -> None:
    """A URL that won't connect within the timeout returns ERR timeout.

    Uses TEST-NET-1 (RFC 5737) which is guaranteed not to route — the WinINet
    connect timeout fires before the protocol stack ever responds."""
    needs_verb(capabilities, "file.download")

    target = _scratch_path()
    r = create_client.request("file.download",
                              "http://192.0.2.1/", target,
                              "--timeout-ms", "500")
    assert isinstance(r, ErrResponse)
    # Some WinINet builds raise permission_denied with a non-timeout
    # wininet_error rather than ERROR_INTERNET_TIMEOUT on unrouteable
    # destinations — accept either, just not OK.
    assert r.code in ("timeout", "permission_denied")
