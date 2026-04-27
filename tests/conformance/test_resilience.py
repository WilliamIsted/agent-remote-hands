"""Fault-tolerance: every error path must keep the agent alive.

These tests deliberately send malformed and over-sized requests, then verify
the agent still answers PING. Failures here mean a bad client could crash
the agent — which is the whole thing the SEH barrier and resource caps are
meant to prevent.
"""
from fixtures import ConformanceTestCase


class ResilienceTests(ConformanceTestCase):
    def test_unknown_verb(self):
        with self.connect() as c:
            c.cmd("AAAAAAAAA BBBBBB")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_empty_line(self):
        """Just a newline. The agent should ignore and keep listening."""
        with self.connect() as c:
            c._send(b"\n")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_oversized_write(self):
        self.require_caps("WRITE")
        with self.connect() as c:
            self.assertTrue(c.cmd("WRITE C:\\x.bin 4000000000").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_oversized_clipset(self):
        self.require_caps("CLIPSET")
        with self.connect() as c:
            self.assertTrue(c.cmd("CLIPSET 4000000000").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_bad_sscanf_args(self):
        with self.connect() as c:
            for bad in [
                "MOVE",
                "MOVE foo bar",
                "MOVEREL",
                "DRAG",
                "WAIT",
                "WAIT notanint",
                "WINMOVE",
                "WINMOVE 0",
                "SHOTRECT 0 0",
            ]:
                if bad.split()[0] not in self.caps():
                    continue
                line = c.cmd(bad)
                self.assertTrue(line.startswith("ERR"),
                                f"{bad!r} did not return ERR: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_missing_path_args(self):
        with self.connect() as c:
            for verb in ("READ", "LIST", "STAT", "DELETE", "MKDIR"):
                if verb not in self.caps():
                    continue
                self.assertTrue(c.cmd(verb).startswith("ERR"),
                                f"{verb} with no arg should ERR")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_long_path_does_not_crash(self):
        self.require_caps("STAT")
        with self.connect() as c:
            long_path = "C:\\" + ("a" * 500) + "\\nope.bin"
            line = c.cmd(f"STAT {long_path}")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_many_consecutive_errors(self):
        """Hit the agent with 100 bad requests in a row — connection must
        survive and PING must still work."""
        with self.connect() as c:
            for _ in range(100):
                c.cmd("BOGUSVERB")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_reconnect_after_quit(self):
        """Closing one connection must not affect the next."""
        with self.connect() as c:
            c.cmd_ok("QUIT")
        with self.connect() as c2:
            self.assertEqual(c2.cmd("PING"), "OK pong")
