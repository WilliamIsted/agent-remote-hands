"""Wire-protocol client for the conformance suite.

Speaks the protocol from PROTOCOL.md directly. Used by the test harness; not
intended as a user-facing CLI (that's `client/hostctl`). Latin-1 on the wire
to match the agent.
"""
import socket


class AgentError(IOError):
    """Raised when the agent returns ERR or the wire protocol is malformed."""


class AgentClient:
    def __init__(self, host, port=8765, timeout=30):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        return self

    def disconnect(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.disconnect()

    def _send(self, data):
        self.sock.sendall(data)

    def _recv_line(self):
        buf = bytearray()
        while True:
            c = self.sock.recv(1)
            if not c:
                return buf.decode("latin-1", errors="replace")
            if c == b"\n":
                return buf.decode("latin-1", errors="replace")
            if c == b"\r":
                continue
            buf += c

    def _recv_n(self, n):
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = self.sock.recv(min(65536, remaining))
            if not chunk:
                raise AgentError(f"connection closed; expected {remaining} more bytes")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def cmd(self, line, payload=None):
        """Send a command. Return the first response line (full text including 'OK'/'ERR')."""
        self._send((line + "\n").encode("latin-1", errors="replace"))
        if payload is not None:
            self._send(payload)
        return self._recv_line()

    def cmd_data(self, line, payload=None):
        """Send a command that returns 'OK [extras...] <length>\\n<bytes>'.

        Returns (extras_list, data_bytes). On ERR, raises AgentError. extras_list
        contains any numeric/string fields between 'OK' and the trailing length
        (e.g. RUN's exit code).
        """
        status = self.cmd(line, payload)
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
        return extras, self._recv_n(length)

    def cmd_ok(self, line, payload=None):
        """Like cmd() but raise AgentError on non-OK response."""
        status = self.cmd(line, payload)
        if not status.startswith("OK"):
            raise AgentError(status)
        return status
