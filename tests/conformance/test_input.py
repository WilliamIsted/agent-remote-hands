"""Mouse and keyboard verbs.

Most input effects can't be observed mechanically without a separate
on-target observer. We verify what we can — MOVE→MPOS, that valid args
return OK, and that invalid args return ERR without crashing the agent.
"""
from fixtures import ConformanceTestCase


class MouseTests(ConformanceTestCase):
    def test_move_then_mpos(self):
        self.require_caps("MOVE", "MPOS", "SCREEN")
        self.require_desktop_session()
        with self.connect() as c:
            scr = c.cmd_ok("SCREEN").split()
            screen_w = int(scr[1])
            screen_h = int(scr[2])
            target_x = min(100, screen_w // 2)
            target_y = min(100, screen_h // 2)
            c.cmd_ok(f"MOVE {target_x} {target_y}")
            line = c.cmd_ok("MPOS")
        parts = line.split()
        # XP/legacy targets may snap cursor to nearest valid pos. Tolerate
        # a few pixels of drift; reject if the agent ignored the request.
        self.assertLess(abs(int(parts[1]) - target_x), 10)
        self.assertLess(abs(int(parts[2]) - target_y), 10)

    def test_move_bad_args(self):
        self.require_caps("MOVE")
        with self.connect() as c:
            self.assertTrue(c.cmd("MOVE").startswith("ERR"))
            self.assertTrue(c.cmd("MOVE foo bar").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_click_returns_ok(self):
        self.require_caps("CLICK")
        with self.connect() as c:
            self.assertTrue(c.cmd("CLICK").startswith("OK"))
            self.assertTrue(c.cmd("CLICK left").startswith("OK"))
            self.assertTrue(c.cmd("CLICK right").startswith("OK"))
            self.assertTrue(c.cmd("CLICK middle").startswith("OK"))

    def test_wheel_returns_ok(self):
        self.require_caps("WHEEL")
        with self.connect() as c:
            self.assertTrue(c.cmd("WHEEL 120").startswith("OK"))
            self.assertTrue(c.cmd("WHEEL -120").startswith("OK"))


class KeyboardTests(ConformanceTestCase):
    def test_key_returns_ok(self):
        """KEY with a known-safe combo (LWIN press alone) returns OK.

        Avoid keys that might have side-effects. LWIN+L would lock the
        screen on Windows; that's destructive. Use VK_F24 instead — a
        defined virtual key that almost no application listens to."""
        self.require_caps("KEY")
        with self.connect() as c:
            self.assertTrue(c.cmd("KEY F24").startswith("OK"))

    def test_key_bad_returns_err(self):
        self.require_caps("KEY")
        with self.connect() as c:
            self.assertTrue(c.cmd("KEY definitely-not-a-key").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_keys_returns_ok(self):
        """KEYS types literal text. Empty arg is valid (typing nothing)."""
        self.require_caps("KEYS")
        with self.connect() as c:
            self.assertTrue(c.cmd("KEYS").startswith("OK"))
