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

"""mDNS discovery + optional TTY selection for the MCP bridge.

When `REMOTE_HANDS_HOST` is set to `mdns:auto` or `mdns:select`, the
bridge resolves the actual host by browsing the
`_remote-hands._tcp.local.` service type instead of using a hard-coded
IP. This makes the bridge resilient to IP shifts between snapshot
reverts, DHCP renewals, or VMware bridged → Hyper-V NAT swaps.

Two modes:

    mdns:auto    Single result → use it.  Zero / multiple → raise with
                 the list (operator must set REMOTE_HANDS_HOST explicitly).

    mdns:select  Single result → use it.  Zero → raise.  Multiple →
                 if a controlling TTY is available, prompt the operator;
                 if not, raise with the list.

The prompt goes through the OS's controlling-terminal device
(`CON:` on Windows, `/dev/tty` on POSIX), NOT through stdin/stdout —
those are reserved for the MCP JSON-RPC channel and any read/write
to them would corrupt the protocol.

This module imports `zeroconf` lazily so the bridge can still start
when the package isn't installed (only fails when `mdns:` is requested).
"""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass
from typing import List, Optional


# ---------------------------------------------------------------------------
# Public API


@dataclass
class DiscoveredAgent:
    host: str           # IP or hostname (.local) resolvable from the host
    port: int
    name: str           # service instance name
    family: str         # "modern" | "legacy" | "classic" — from os= TXT field
    raw_txt: dict       # full TXT-record map for diagnostics


class MdnsError(RuntimeError):
    """Raised when mDNS discovery cannot resolve a single host.

    The message is suitable for surfacing to the operator (the bridge
    logs it to stderr and exits — MCP clients show this in the server-
    error UI)."""


def resolve(host_spec: str, *, timeout_s: float = 3.0) -> str:
    """Resolve a `REMOTE_HANDS_HOST` spec to a concrete host string.

    - Non-mdns spec → returned unchanged.
    - `mdns:auto`   → run discovery, return single match's host (or raise).
    - `mdns:select` → run discovery, prompt via TTY when multiple
                      (or raise when TTY unavailable + multi-result).

    Other returns from this function should be safe to pass to
    `socket.create_connection((host, port))`.
    """
    spec = host_spec.strip()
    if not spec.startswith("mdns:"):
        return spec

    mode = spec.split(":", 1)[1] or "auto"
    if mode not in ("auto", "select"):
        raise MdnsError(
            f"REMOTE_HANDS_HOST: unknown mdns mode {mode!r} "
            f"(expected 'mdns:auto' or 'mdns:select')")

    agents = _discover(timeout_s)
    if not agents:
        raise MdnsError(
            "mDNS discovery found 0 agents advertising "
            "`_remote-hands._tcp.local.`. Is the agent running with "
            "mDNS enabled (default)? Check firewall UDP/5353 inbound.")

    if len(agents) == 1:
        return agents[0].host

    # Multiple — behaviour diverges by mode.
    if mode == "auto":
        listing = _format_agent_list(agents)
        raise MdnsError(
            f"mDNS discovery found {len(agents)} agents — "
            f"`mdns:auto` requires exactly one. Either set "
            f"REMOTE_HANDS_HOST to a specific host below, or use "
            f"`mdns:select` to be prompted:\n{listing}")

    # mdns:select with multi-result → try TTY
    selected = _prompt_via_tty(agents)
    if selected is None:
        listing = _format_agent_list(agents)
        raise MdnsError(
            f"mDNS discovery found {len(agents)} agents but no "
            f"controlling terminal is available for selection. Set "
            f"REMOTE_HANDS_HOST to one of:\n{listing}")
    return selected.host


# ---------------------------------------------------------------------------
# Discovery (private)


def _discover(timeout_s: float) -> List[DiscoveredAgent]:
    """Browse `_remote-hands._tcp.local.` for `timeout_s` seconds and
    return all distinct service instances seen.

    `zeroconf` is imported lazily so a fresh install without the dep
    can still start the bridge (the import-error message points the
    operator at `pip install zeroconf`)."""
    try:
        from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
    except ImportError as e:
        raise MdnsError(
            "Python `zeroconf` package not installed. Add it via "
            "`pip install zeroconf` (or `pip install -r mcp-server/"
            "requirements.txt`)."
        ) from e

    found: List[DiscoveredAgent] = []
    seen_names: set[str] = set()

    class _Listener(ServiceListener):
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=int(timeout_s * 1000))
            if info is None or name in seen_names:
                return
            seen_names.add(name)
            # zeroconf returns parsed_addresses with IPv4 + IPv6.  Prefer
            # IPv4 first since the agent's IPv6 listener may be absent.
            host = None
            for addr in info.parsed_addresses():
                if ":" not in addr:  # IPv4
                    host = addr
                    break
            if host is None and info.parsed_addresses():
                host = info.parsed_addresses()[0]
            if host is None:
                return
            txt = {}
            if info.properties:
                for k, v in info.properties.items():
                    try:
                        kk = k.decode("ascii", "replace") if isinstance(k, bytes) else str(k)
                        vv = v.decode("ascii", "replace") if isinstance(v, bytes) else str(v)
                        txt[kk] = vv
                    except Exception:
                        pass
            found.append(DiscoveredAgent(
                host=host,
                port=info.port,
                name=name,
                family=txt.get("os", "unknown"),
                raw_txt=txt,
            ))

        def update_service(self, zc, type_, name): pass
        def remove_service(self, zc, type_, name): pass

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, "_remote-hands._tcp.local.", _Listener())
        time.sleep(timeout_s)
    finally:
        zc.close()
    return found


def _format_agent_list(agents: List[DiscoveredAgent]) -> str:
    """Pretty-print the discovered agents for an error message or prompt."""
    lines = []
    for i, a in enumerate(agents, start=1):
        lines.append(f"  [{i}]  {a.host}:{a.port}  family={a.family}  name={a.name}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# TTY selection (private)


def _prompt_via_tty(agents: List[DiscoveredAgent]) -> Optional[DiscoveredAgent]:
    """Open the controlling terminal and prompt the operator to pick.

    Returns the chosen DiscoveredAgent, or None when no controlling
    terminal is available. NEVER touches stdin / stdout — those are
    reserved for MCP JSON-RPC. Max 3 invalid responses before giving up.
    """
    tty = _open_tty()
    if tty is None:
        return None
    try:
        tty.write("\nMultiple Agent Remote Hands agents discovered:\n")
        tty.write(_format_agent_list(agents) + "\n")
        for attempt in range(3):
            tty.write(f"Select [1-{len(agents)}]: ")
            tty.flush()
            line = tty.readline()
            if not line:
                return None
            line = line.strip()
            if line.isdigit():
                idx = int(line)
                if 1 <= idx <= len(agents):
                    return agents[idx - 1]
            tty.write("Invalid selection.\n")
        tty.write("Too many invalid attempts.\n")
        return None
    finally:
        try:
            tty.close()
        except Exception:
            pass


def _open_tty():
    """Open the OS's controlling-terminal device for read+write.

    Returns a text-mode file object, or None if no controlling
    terminal is available (e.g. running under Claude Desktop GUI
    with no terminal attached, or stdin/stdout redirected without
    a TTY behind them).
    """
    # Windows: CON: is the console device. Open it explicitly to bypass
    # any stdin/stdout redirection.
    if sys.platform == "win32":
        try:
            return open("CON:", "r+", encoding="utf-8", errors="replace")
        except OSError:
            return None
    # POSIX: /dev/tty is the controlling-terminal device.
    try:
        return open("/dev/tty", "r+", encoding="utf-8", errors="replace")
    except OSError:
        return None
