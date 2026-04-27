"""Filesystem verbs: READ, WRITE, LIST, STAT, DELETE, MKDIR, RENAME."""
import os
import time

from client import AgentError
from fixtures import ConformanceTestCase


class FileTests(ConformanceTestCase):
    def test_write_then_read_roundtrip(self):
        self.require_caps("WRITE", "READ")
        ws = self.workspace()
        path = self.path_join(ws, "roundtrip.bin")
        payload = bytes(range(256)) * 8  # 2048 bytes, includes \n and binary
        with self.connect() as c:
            c.cmd_ok(f"WRITE {path} {len(payload)}", payload)
            extras, got = c.cmd_data(f"READ {path}")
            self.assertEqual(got, payload)

    def test_write_empty_file(self):
        self.require_caps("WRITE", "READ")
        ws = self.workspace()
        path = self.path_join(ws, "empty.bin")
        with self.connect() as c:
            c.cmd_ok(f"WRITE {path} 0", b"")
            extras, got = c.cmd_data(f"READ {path}")
            self.assertEqual(got, b"")

    def test_stat_size_matches_write(self):
        self.require_caps("WRITE", "STAT")
        ws = self.workspace()
        path = self.path_join(ws, "stat.bin")
        payload = b"x" * 1234
        with self.connect() as c:
            c.cmd_ok(f"WRITE {path} {len(payload)}", payload)
            line = c.cmd_ok(f"STAT {path}")
        # OK <type> <size> <mtime>
        parts = line.split()
        self.assertEqual(parts[0], "OK")
        self.assertEqual(parts[1], "F")
        self.assertEqual(int(parts[2]), len(payload))

    def test_stat_missing_returns_err(self):
        self.require_caps("STAT")
        with self.connect() as c:
            line = c.cmd("STAT C:\\does-not-exist-rh-conf-zzz.tmp")
            self.assertTrue(line.startswith("ERR"))

    def test_list_workspace(self):
        self.require_caps("WRITE", "MKDIR", "LIST")
        ws = self.workspace()
        with self.connect() as c:
            for name in ("a.txt", "b.txt", "c.txt"):
                c.cmd_ok(f"WRITE {self.path_join(ws, name)} 1", b"x")
            c.cmd_ok(f"MKDIR {self.path_join(ws, 'subdir')}")
            extras, data = c.cmd_data(f"LIST {ws}")
        rows = data.decode("latin-1").splitlines()
        names = set()
        types = {}
        for r in rows:
            fields = r.split("\t")
            self.assertEqual(len(fields), 4, f"row must have 4 tab-separated fields: {r!r}")
            type_, size_, mtime_, name_ = fields
            self.assertIn(type_, ("F", "D", "L", "?"))
            int(size_)   # parses as int
            int(mtime_)  # parses as int (signed; pre-1970 mtimes go negative)
            names.add(name_)
            types[name_] = type_
        for n in ("a.txt", "b.txt", "c.txt", "subdir"):
            self.assertIn(n, names, f"{n} missing from LIST output")
        self.assertEqual(types["subdir"], "D")
        self.assertEqual(types["a.txt"], "F")
        self.assertNotIn(".", names, "LIST must omit . entry")
        self.assertNotIn("..", names, "LIST must omit .. entry")

    def test_mkdir_then_delete(self):
        self.require_caps("MKDIR", "DELETE", "STAT")
        ws = self.workspace()
        path = self.path_join(ws, "tobedeleted")
        with self.connect() as c:
            c.cmd_ok(f"MKDIR {path}")
            self.assertTrue(c.cmd(f"STAT {path}").startswith("OK D"))
            c.cmd_ok(f"DELETE {path}")
            self.assertTrue(c.cmd(f"STAT {path}").startswith("ERR"))

    def test_delete_file(self):
        self.require_caps("WRITE", "DELETE", "STAT")
        ws = self.workspace()
        path = self.path_join(ws, "tobedeleted.bin")
        with self.connect() as c:
            c.cmd_ok(f"WRITE {path} 3", b"abc")
            c.cmd_ok(f"DELETE {path}")
            self.assertTrue(c.cmd(f"STAT {path}").startswith("ERR"))

    def test_rename_simple(self):
        self.require_caps("WRITE", "RENAME", "STAT")
        ws = self.workspace()
        src = self.path_join(ws, "renamesrc.bin")
        dst = self.path_join(ws, "renamedst.bin")
        with self.connect() as c:
            c.cmd_ok(f"WRITE {src} 3", b"abc")
            c.cmd_ok(f"RENAME {src} {dst}")
            self.assertTrue(c.cmd(f"STAT {dst}").startswith("OK F"))
            self.assertTrue(c.cmd(f"STAT {src}").startswith("ERR"))

    def test_rename_paths_with_spaces_via_tab(self):
        """RENAME uses tab as separator when a path contains spaces."""
        self.require_caps("WRITE", "RENAME", "STAT")
        ws = self.workspace()
        src = self.path_join(ws, "src with space.bin")
        dst = self.path_join(ws, "dst with space.bin")
        with self.connect() as c:
            c.cmd_ok(f"WRITE {src} 3", b"abc")
            c.cmd_ok(f"RENAME {src}\t{dst}")
            self.assertTrue(c.cmd(f"STAT {dst}").startswith("OK F"))

    def test_write_oversized_returns_err_not_crash(self):
        """A WRITE header claiming a multi-GB payload must be rejected without
        the agent attempting to malloc it. Connection must survive."""
        self.require_caps("WRITE")
        with self.connect() as c:
            line = c.cmd("WRITE C:\\rh-conf-bogus.bin 4000000000")
            self.assertTrue(line.startswith("ERR"), f"expected ERR, got: {line}")
            # Verify connection survives
            self.assertEqual(c.cmd("PING"), "OK pong")
