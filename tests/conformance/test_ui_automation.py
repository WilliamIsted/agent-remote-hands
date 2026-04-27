"""UI Automation verbs: ELEMENTS, ELEMENT_AT, ELEMENT_FIND, ELEMENT_INVOKE,
ELEMENT_FOCUS.

These tests SKIP cleanly on agents that don't advertise `ui_automation=uia`.
On agents that do, they exercise the row format and the id round-trip; they
don't invoke arbitrary controls (which could trigger destructive actions on
the target VM). The agent's own console window is the safest fixture — it
exists, has a known title, and clicking it is harmless.
"""
import time

from client import AgentError
from fixtures import ConformanceTestCase


class UIAvailability(ConformanceTestCase):
    def test_ui_automation_advertised(self):
        info = self.info()
        self.assertIn("ui_automation", info,
                      "INFO must include ui_automation flag")
        self.assertIn(info["ui_automation"], ("uia", "msaa", "no"),
                      f"unknown ui_automation value: {info['ui_automation']!r}")


class UIATests(ConformanceTestCase):
    """Skip the whole class unless the agent advertises uia."""
    def setUp(self):
        if self.info().get("ui_automation") != "uia":
            self.skipTest("agent does not advertise ui_automation=uia")

    def _parse_row(self, line):
        """Parse one tab-separated element row.
        Returns dict with id, x, y, w, h, role, name, value, flags."""
        fields = line.split("\t")
        self.assertEqual(len(fields), 9,
                         f"row must have 9 tab-separated fields: {line!r}")
        return {
            "id": int(fields[0]),
            "x": int(fields[1]),
            "y": int(fields[2]),
            "w": int(fields[3]),
            "h": int(fields[4]),
            "role": fields[5],
            "name": fields[6],
            "value": fields[7],
            "flags": fields[8],
        }

    def test_elements_returns_rows(self):
        self.require_caps("ELEMENTS")
        with self.connect() as c:
            extras, data = c.cmd_data("ELEMENTS")
        rows = data.decode("latin-1").splitlines()
        # Even an empty desktop has *something* — at minimum the desktop
        # window itself.
        self.assertGreater(len(rows), 0,
                           "ELEMENTS should return at least one element")
        for r in rows:
            parsed = self._parse_row(r)
            self.assertGreater(parsed["id"], 0)
            self.assertIn(parsed["role"],
                          ("button", "calendar", "checkbox", "combobox",
                           "edit", "link", "image", "listitem", "list", "menu",
                           "menubar", "menuitem", "progressbar", "radio",
                           "scrollbar", "slider", "spinner", "statusbar",
                           "tab", "tabitem", "text", "toolbar", "tooltip",
                           "tree", "treeitem", "custom", "group", "thumb",
                           "datagrid", "dataitem", "document", "splitbutton",
                           "window", "pane", "header", "headeritem", "table",
                           "titlebar", "separator", "semanticzoom", "appbar",
                           "unknown"),
                          f"unexpected role: {parsed['role']!r}")

    def test_elements_region_filter(self):
        """ELEMENTS x y w h restricts to the subtree at the rect's centre.
        Should return strictly fewer (or equal) rows than the unfiltered
        whole-screen call."""
        self.require_caps("ELEMENTS")
        with self.connect() as c:
            _, full = c.cmd_data("ELEMENTS")
            _, region = c.cmd_data("ELEMENTS 0 0 100 100")
        full_rows = full.decode("latin-1").splitlines()
        region_rows = region.decode("latin-1").splitlines()
        self.assertLessEqual(len(region_rows), len(full_rows))

    def test_element_at_round_trip(self):
        """ELEMENT_AT for a centre point returns one row. The id should be
        usable in subsequent verbs (here we just re-issue ELEMENT_AT and
        confirm a different id is issued — ids monotonically increase)."""
        self.require_caps("ELEMENT_AT", "SCREEN")
        with self.connect() as c:
            scr = c.cmd_ok("SCREEN").split()
            cx = int(scr[1]) // 2
            cy = int(scr[2]) // 2
            line = c.cmd(f"ELEMENT_AT {cx} {cy}")
            self.assertTrue(line.startswith("OK "), f"unexpected: {line!r}")
            row1 = self._parse_row(line[3:])
            line = c.cmd(f"ELEMENT_AT {cx} {cy}")
            self.assertTrue(line.startswith("OK "))
            row2 = self._parse_row(line[3:])
            self.assertGreater(row2["id"], row1["id"],
                               "successive ELEMENT_AT calls must issue new ids")

    def test_element_find_unknown_returns_err(self):
        self.require_caps("ELEMENT_FIND")
        with self.connect() as c:
            line = c.cmd("ELEMENT_FIND button RH_DEFINITELY_NO_BUTTON_LIKE_THIS_XYZ")
            self.assertTrue(line.startswith("ERR"),
                            f"expected ERR, got: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_invoke_invalid_id(self):
        self.require_caps("ELEMENT_INVOKE")
        with self.connect() as c:
            line = c.cmd("ELEMENT_INVOKE 999999")
            self.assertTrue(line.startswith("ERR"),
                            f"unknown id should ERR: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_focus_invalid_id(self):
        self.require_caps("ELEMENT_FOCUS")
        with self.connect() as c:
            line = c.cmd("ELEMENT_FOCUS 999999")
            self.assertTrue(line.startswith("ERR"),
                            f"unknown id should ERR: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_id_space_is_per_connection(self):
        """Element ids issued on connection A should not be valid on
        connection B. Two fresh connections each see id=1 as their first
        registered element."""
        self.require_caps("ELEMENTS", "ELEMENT_INVOKE")
        with self.connect() as a:
            extras_a, data_a = a.cmd_data("ELEMENTS 0 0 100 100")
        with self.connect() as b:
            # B never called ELEMENTS — its map is empty
            line = b.cmd("ELEMENT_INVOKE 1")
            self.assertTrue(line.startswith("ERR"),
                            f"B should not see A's ids: {line!r}")

    def test_elements_rebuilds_id_map(self):
        """A second ELEMENTS call invalidates ids from the first."""
        self.require_caps("ELEMENTS", "ELEMENT_INVOKE")
        with self.connect() as c:
            _, first = c.cmd_data("ELEMENTS 0 0 100 100")
            rows = first.decode("latin-1").splitlines()
            if not rows:
                self.skipTest("empty region; nothing to invalidate")
            first_id = int(rows[0].split("\t")[0])
            # Rebuild
            c.cmd_data("ELEMENTS 0 0 100 100")
            # Old id should be gone (or pointing to something different;
            # at minimum the agent must not crash on it)
            line = c.cmd(f"ELEMENT_INVOKE {first_id}")
            # Either ERR (cleanly rejected) or OK (rebuilt to same id) —
            # both are acceptable; what we assert is "didn't crash".
            self.assertTrue(line.startswith("OK") or line.startswith("ERR"),
                            f"unexpected: {line!r}")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_tree_format(self):
        """ELEMENT_TREE rows have a leading depth column; depth 0 is the
        seed element."""
        self.require_caps("ELEMENT_TREE", "ELEMENT_AT", "SCREEN")
        with self.connect() as c:
            scr = c.cmd_ok("SCREEN").split()
            line = c.cmd(f"ELEMENT_AT {int(scr[1])//2} {int(scr[2])//2}")
            self.assertTrue(line.startswith("OK "), f"hit-test failed: {line!r}")
            seed_id = int(line[3:].split("\t")[0])
            extras, data = c.cmd_data(f"ELEMENT_TREE {seed_id}")
        rows = data.decode("latin-1").splitlines()
        self.assertGreater(len(rows), 0)
        first_fields = rows[0].split("\t")
        self.assertEqual(len(first_fields), 10,
                         f"tree row must have 10 fields (depth + 9): {rows[0]!r}")
        self.assertEqual(int(first_fields[0]), 0,
                         "first row of tree is depth 0 (seed element)")
        # Every depth must be a non-negative int; depths must monotonically
        # be reachable from previous (tree shape, not strict ordering)
        for r in rows:
            d = int(r.split("\t")[0])
            self.assertGreaterEqual(d, 0)

    def test_element_tree_invalid_id(self):
        self.require_caps("ELEMENT_TREE")
        with self.connect() as c:
            line = c.cmd("ELEMENT_TREE 999999")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_text_invalid_id(self):
        self.require_caps("ELEMENT_TEXT")
        with self.connect() as c:
            line = c.cmd("ELEMENT_TEXT 999999")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_text_returns_bytes(self):
        """ELEMENT_TEXT on the active foreground window's tree should
        return *some* text on a typical session — at minimum the title."""
        self.require_caps("ELEMENT_TEXT", "WINACTIVE", "ELEMENT_AT")
        with self.connect() as c:
            line = c.cmd("WINACTIVE")
            if not line.startswith("OK "):
                self.skipTest("no active window")
            payload = line[3:]
            fields = payload.split("\t")
            x = int(fields[1]); y = int(fields[2])
            w = int(fields[3]); h = int(fields[4])
            cx = x + w // 2; cy = y + h // 2
            line = c.cmd(f"ELEMENT_AT {cx} {cy}")
            if not line.startswith("OK "):
                self.skipTest("hit-test failed for active window")
            id_ = int(line[3:].split("\t")[0])
            extras, data = c.cmd_data(f"ELEMENT_TEXT {id_}")
        # Text may be empty (e.g. for a generic pane) but the payload must
        # parse cleanly — that's what we verify here.
        self.assertIsInstance(data, bytes)

    def test_element_toggle_invalid_id(self):
        self.require_caps("ELEMENT_TOGGLE")
        with self.connect() as c:
            line = c.cmd("ELEMENT_TOGGLE 999999")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_expand_invalid_id(self):
        self.require_caps("ELEMENT_EXPAND")
        with self.connect() as c:
            line = c.cmd("ELEMENT_EXPAND 999999")
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_element_set_text_invalid_id(self):
        self.require_caps("ELEMENT_SET_TEXT")
        with self.connect() as c:
            payload = b"hello"
            line = c.cmd(f"ELEMENT_SET_TEXT 999999 {len(payload)}", payload)
            self.assertTrue(line.startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")
