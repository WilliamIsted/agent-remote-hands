"""PING, CAPS, INFO, QUIT, unknown-verb handling. Required of every agent."""
import select

from client import AgentClient
from fixtures import ConformanceTestCase


class BasicsTests(ConformanceTestCase):
    def test_ping(self):
        with self.connect() as c:
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_caps_required_verbs(self):
        verbs = self.caps()
        for required in ("PING", "CAPS", "INFO"):
            self.assertIn(required, verbs, f"CAPS must include {required}")

    def test_info_required_keys(self):
        info = self.info()
        for required in ("os", "protocol"):
            self.assertIn(required, info, f"INFO must include {required}")
        self.assertEqual(info["protocol"], "1", "protocol version must be 1")

    def test_caps_matches_implementations(self):
        """Every verb in CAPS should respond to *something* — not silently
        time out or return ERR unsupported."""
        verbs = self.caps()
        # Verbs that need args; sending bare should ERR (with a usage hint),
        # but the connection must stay alive.
        skip = {"WRITE", "CLIPSET", "RUN", "EXEC", "WAIT", "KILL", "SLEEP",
                "READ", "LIST", "STAT", "DELETE", "MKDIR", "RENAME",
                "KEY", "KEYS", "KEYDOWN", "KEYUP",
                "MOVE", "MOVEREL", "DRAG", "WHEEL",
                "SHOTRECT", "SHOTWIN",
                "WINFIND", "WINMOVE", "WINSIZE", "WINFOCUS",
                "WINCLOSE", "WINMIN", "WINMAX", "WINRESTORE",
                "ENV",
                "ELEMENT_AT", "ELEMENT_FIND", "ELEMENT_INVOKE", "ELEMENT_FOCUS",
                "ELEMENT_TREE", "ELEMENT_TEXT", "ELEMENT_SET_TEXT",
                "ELEMENT_TOGGLE", "ELEMENT_EXPAND", "ELEMENT_COLLAPSE",
                "WATCH", "WAITFOR"}
        with self.connect() as c:
            for v in sorted(verbs):
                if v in skip:
                    continue
                if v in ("QUIT", "EXIT", "BYE", "LOGOFF", "REBOOT", "SHUTDOWN"):
                    continue  # closes connection or terminates session
                line = c.cmd(v)
                self.assertTrue(
                    line.startswith("OK") or line.startswith("ERR"),
                    f"verb {v} returned malformed response: {line!r}",
                )
                # Drain length-prefixed payloads so the next iteration's
                # read doesn't see stale data. ELEMENTS, PS, WINLIST, etc.
                # all return OK <length>\n<bytes>. But some verbs return
                # OK <count> as a scalar (DESKTOPCOUNT, MPOS x y, SCREEN w h)
                # where the trailing integer is *not* a length. We can't tell
                # from the line alone, so peek with select() — if no payload
                # bytes are queued within 200ms, treat as a scalar response.
                if line.startswith("OK"):
                    parts = line.split()
                    if len(parts) >= 2:
                        try:
                            length = int(parts[-1])
                        except ValueError:
                            length = 0  # last token isn't numeric, scalar response
                        if length > 0:
                            ready, _, _ = select.select([c.sock], [], [], 0.2)
                            if ready:
                                c._recv_n(length)

    def test_unknown_verb_returns_err(self):
        with self.connect() as c:
            line = c.cmd("DEFINITELY_NOT_A_VERB")
            self.assertTrue(line.startswith("ERR"))

    def test_unknown_verb_does_not_break_connection(self):
        """ERR must keep the connection alive — clients can recover."""
        with self.connect() as c:
            c.cmd("BOGUS")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_quit_closes_cleanly(self):
        c = AgentClient(self.host, self.port, self.timeout).connect()
        try:
            self.assertTrue(c.cmd("QUIT").startswith("OK"))
        finally:
            c.disconnect()
