"""System verbs: ENV, IDLE, DRIVES, LOCK."""
from fixtures import ConformanceTestCase


class SystemTests(ConformanceTestCase):
    def test_env_single_known_var(self):
        self.require_caps("ENV")
        # Pick a variable that exists on every Windows session.
        var = "PATH" if self.info().get("os", "").startswith("windows") else "PATH"
        with self.connect() as c:
            line = c.cmd(f"ENV {var}")
        self.assertTrue(line.startswith("OK "), f"ENV {var} returned: {line!r}")
        self.assertGreater(len(line), 4, "PATH should be non-empty")

    def test_env_unset_returns_err(self):
        self.require_caps("ENV")
        with self.connect() as c:
            self.assertTrue(c.cmd("ENV RH_DEFINITELY_UNSET_XYZ").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_env_full_dump(self):
        self.require_caps("ENV")
        with self.connect() as c:
            extras, data = c.cmd_data("ENV")
        rows = data.decode("latin-1").splitlines()
        self.assertGreater(len(rows), 0)
        for r in rows:
            self.assertIn("=", r, f"ENV row missing '=': {r!r}")

    def test_idle_returns_int(self):
        self.require_caps("IDLE")
        with self.connect() as c:
            line = c.cmd_ok("IDLE")
        seconds = int(line.split()[1])
        self.assertGreaterEqual(seconds, 0)

    def test_drives_format(self):
        self.require_caps("DRIVES")
        with self.connect() as c:
            extras, data = c.cmd_data("DRIVES")
        rows = data.decode("latin-1").splitlines()
        self.assertGreater(len(rows), 0)
        valid_types = {"fixed", "removable", "remote", "cdrom", "ramdisk", "unknown"}
        for r in rows:
            fields = r.split("\t")
            self.assertEqual(len(fields), 2, f"DRIVES row must be path<TAB>type: {r!r}")
            self.assertIn(fields[1], valid_types,
                          f"unknown drive type: {fields[1]!r}")
