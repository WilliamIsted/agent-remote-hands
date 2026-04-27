"""Async wire-protocol client for the agent.

Mirror of tests/conformance/client.py but built on asyncio so the MCP
server (which is async-first via the SDK) doesn't have to wrap blocking
sockets in threads. Wire format is identical — `OK <length>\\n<bytes>`,
multi-row responses, multi-frame streams — see PROTOCOL.md for the spec.
"""
import asyncio
import os
import sys


# Optional wire-level tracing. Set REMOTE_HANDS_TRACE=1 to print every
# command sent and response received to stderr - useful when running
# under an MCP client to surface what's hitting the agent.
_TRACE = os.environ.get("REMOTE_HANDS_TRACE") == "1"


def _trace(direction: str, line: str) -> None:
    if _TRACE:
        print(f"[wire {direction}] {line}", file=sys.stderr, flush=True)


class AgentError(IOError):
    """Raised when the agent returns ERR or the wire protocol is malformed."""


class AsyncAgentClient:
    """Single TCP connection to one agent. One outstanding command at a
    time — a connection is a serialized command channel, not a multiplex.
    The MCP server uses a `asyncio.Lock` around `cmd*` calls.
    """

    def __init__(self, host: str, port: int = 8765, timeout: float = 30.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None

    async def connect(self) -> None:
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port),
            timeout=self.timeout,
        )

    async def disconnect(self) -> None:
        if self.writer is not None:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except (OSError, asyncio.TimeoutError):
                pass
            self.writer = None
            self.reader = None

    @property
    def connected(self) -> bool:
        return self.writer is not None and not self.writer.is_closing()

    async def _send(self, data: bytes) -> None:
        assert self.writer is not None
        self.writer.write(data)
        await self.writer.drain()

    async def _recv_line(self) -> str:
        assert self.reader is not None
        buf = bytearray()
        while True:
            c = await self.reader.read(1)
            if not c:
                return buf.decode("latin-1", errors="replace")
            if c == b"\n":
                return buf.decode("latin-1", errors="replace")
            if c == b"\r":
                continue
            buf += c

    async def _recv_n(self, n: int) -> bytes:
        assert self.reader is not None
        if n <= 0:
            return b""
        return await self.reader.readexactly(n)

    async def cmd(self, line: str, payload: bytes | None = None) -> str:
        """Send one command, return the first response line."""
        _trace("->", line + (f" (+{len(payload)} bytes)" if payload else ""))
        await self._send((line + "\n").encode("latin-1", errors="replace"))
        if payload is not None:
            await self._send(payload)
        resp = await self._recv_line()
        _trace("<-", resp)
        return resp

    async def cmd_ok(self, line: str, payload: bytes | None = None) -> str:
        """Like cmd() but raise AgentError on ERR."""
        status = await self.cmd(line, payload)
        if not status.startswith("OK"):
            raise AgentError(status)
        return status

    async def cmd_data(
        self, line: str, payload: bytes | None = None
    ) -> tuple[list[str], bytes]:
        """Send a command that returns 'OK [extras...] <length>\\n<bytes>'.

        Returns (extras_list, data_bytes). Raises AgentError on ERR. extras
        contains positional metadata fields that some verbs prefix before
        the length (e.g. RUN's exit code).
        """
        status = await self.cmd(line, payload)
        if not status.startswith("OK"):
            raise AgentError(status)
        parts = status.split()
        if len(parts) < 2:
            raise AgentError(f"malformed OK line: {status!r}")
        try:
            length = int(parts[-1])
        except ValueError:
            raise AgentError(f"non-integer length in: {status!r}")
        extras = parts[1:-1]
        return extras, await self._recv_n(length)

    async def query_caps(self) -> set[str]:
        """Read CAPS and parse into a set of verb names."""
        line = await self.cmd("CAPS")
        if not line.startswith("OK"):
            return set()
        return set(line.split()[1:])

    async def query_info(self) -> dict[str, str]:
        """Read INFO and parse into a key=value dict."""
        line = await self.cmd("INFO")
        if not line.startswith("OK"):
            return {}
        kv: dict[str, str] = {}
        for tok in line.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        return kv
