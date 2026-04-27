"""Top-level window verbs: WINLIST, WINFIND, WINACTIVE, WIN* control."""
from fixtures import ConformanceTestCase


class WindowsTests(ConformanceTestCase):
    def setUp(self):
        if self.info().get("windows") != "yes":
            self.skipTest("agent reports windows=no")

    def test_winlist_format(self):
        self.require_caps("WINLIST")
        with self.connect() as c:
            extras, data = c.cmd_data("WINLIST")
        rows = data.decode("latin-1").splitlines()
        # Empty desktop is unusual but legal — only check format if rows exist
        for r in rows:
            fields = r.split("\t")
            self.assertEqual(len(fields), 6,
                             f"WINLIST row must have 6 tab-separated fields: {r!r}")
            int(fields[0])  # hwnd
            int(fields[1])  # x
            int(fields[2])  # y
            int(fields[3])  # w
            int(fields[4])  # h
            # title can be empty in theory but is filtered out by the agent

    def test_winactive_format(self):
        self.require_caps("WINACTIVE")
        with self.connect() as c:
            line = c.cmd("WINACTIVE")
        # Either OK <hwnd>\t... or ERR (no foreground window — possible
        # on a freshly logged-in session before anything claims focus)
        if line.startswith("OK"):
            payload = line[3:]
            fields = payload.split("\t")
            self.assertEqual(len(fields), 6,
                             f"WINACTIVE must return 6 tab-separated fields: {line!r}")
            int(fields[0])  # hwnd
            int(fields[1])  # x
            int(fields[2])  # y
            int(fields[3])  # w
            int(fields[4])  # h
        else:
            self.assertTrue(line.startswith("ERR"))

    def test_winfind_missing_returns_err(self):
        self.require_caps("WINFIND")
        with self.connect() as c:
            line = c.cmd("WINFIND DefinitelyNoSuchWindowTitle12345")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")
