"""Process management: EXEC, RUN, WAIT, KILL, PS, SLEEP."""
import time

from fixtures import ConformanceTestCase


class ProcessTests(ConformanceTestCase):
    def _shell_echo(self, text):
        """Build a cmdline that prints `text` to stdout — uses cmd /c on
        Windows targets, sh -c elsewhere."""
        if self.info().get("os", "").startswith("windows"):
            return f'cmd /c echo {text}'
        return f"/bin/sh -c 'echo {text}'"

    def _shell_exit(self, code):
        if self.info().get("os", "").startswith("windows"):
            return f'cmd /c exit {code}'
        return f"/bin/sh -c 'exit {code}'"

    def test_run_captures_stdout(self):
        self.require_caps("RUN")
        with self.connect() as c:
            extras, data = c.cmd_data(self._run_line(self._shell_echo("hello-conformance")))
        self.assertEqual(len(extras), 1, "RUN must return 'OK <exit> <length>'")
        self.assertEqual(extras[0], "0", "echo should exit 0")
        self.assertIn(b"hello-conformance", data)

    def _run_line(self, cmdline):
        return f"RUN {cmdline}"

    def test_run_nonzero_exit(self):
        self.require_caps("RUN")
        with self.connect() as c:
            extras, _ = c.cmd_data(self._run_line(self._shell_exit(7)))
        self.assertEqual(extras[0], "7")

    def test_exec_returns_pid(self):
        self.require_caps("EXEC")
        with self.connect() as c:
            line = c.cmd_ok(self._shell_exit_via_exec(0))
        parts = line.split()
        self.assertEqual(parts[0], "OK")
        pid = int(parts[1])
        self.assertGreater(pid, 0)

    def _shell_exit_via_exec(self, code):
        if self.info().get("os", "").startswith("windows"):
            return f'EXEC cmd /c exit {code}'
        return f"EXEC /bin/sh -c 'exit {code}'"

    def test_exec_then_wait(self):
        self.require_caps("EXEC", "WAIT")
        with self.connect() as c:
            line = c.cmd_ok(self._shell_exit_via_exec(0))
            pid = int(line.split()[1])
            wait_line = c.cmd_ok(f"WAIT {pid} 5000")
        self.assertEqual(wait_line.split()[1], "0")

    def test_exec_wait_timeout(self):
        """EXEC a long-running process, WAIT briefly — should ERR timeout
        without crashing the agent."""
        self.require_caps("EXEC", "WAIT", "KILL")
        if not self.info().get("os", "").startswith("windows"):
            self.skipTest("only verifying the windows path")
        with self.connect() as c:
            # ping localhost a few times — guaranteed to take at least 4s
            line = c.cmd_ok("EXEC ping -n 5 127.0.0.1")
            pid = int(line.split()[1])
            wait_line = c.cmd(f"WAIT {pid} 100")
            self.assertTrue(wait_line.startswith("ERR"))
            # Clean up — kill the long-runner so it doesn't outlive the test
            c.cmd(f"KILL {pid}")

    def test_kill_invalid_pid_returns_err(self):
        self.require_caps("KILL")
        with self.connect() as c:
            self.assertTrue(c.cmd("KILL 0").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_ps_lists_processes(self):
        self.require_caps("PS")
        with self.connect() as c:
            extras, data = c.cmd_data("PS")
        rows = data.decode("latin-1").splitlines()
        self.assertGreater(len(rows), 0)
        for r in rows:
            fields = r.split("\t")
            self.assertEqual(len(fields), 2, f"PS row must be pid<TAB>name: {r!r}")
            int(fields[0])  # pid is integer
            self.assertGreater(len(fields[1]), 0)

    def test_sleep_blocks(self):
        self.require_caps("SLEEP")
        with self.connect() as c:
            t0 = time.time()
            c.cmd_ok("SLEEP 200")
            elapsed = time.time() - t0
        self.assertGreaterEqual(elapsed, 0.15)
        self.assertLess(elapsed, 5.0, "SLEEP 200 should take ~0.2s, not seconds")
