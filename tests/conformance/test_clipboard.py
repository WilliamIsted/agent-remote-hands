"""Clipboard: CLIPSET, CLIPGET."""
from fixtures import ConformanceTestCase


class ClipboardTests(ConformanceTestCase):
    def test_set_then_get_roundtrip(self):
        self.require_caps("CLIPSET", "CLIPGET")
        payload = b"Hello, conformance test! Various ASCII chars: !@#$%^&*()"
        with self.connect() as c:
            c.cmd_ok(f"CLIPSET {len(payload)}", payload)
            extras, got = c.cmd_data("CLIPGET")
        self.assertEqual(got, payload)

    def test_set_multiline(self):
        self.require_caps("CLIPSET", "CLIPGET")
        payload = b"line one\nline two\nline three"
        with self.connect() as c:
            c.cmd_ok(f"CLIPSET {len(payload)}", payload)
            extras, got = c.cmd_data("CLIPGET")
        self.assertEqual(got, payload)

    def test_set_oversized_returns_err(self):
        self.require_caps("CLIPSET")
        with self.connect() as c:
            line = c.cmd("CLIPSET 4000000000")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")
