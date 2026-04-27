"""Multi-connection behaviour: independent dispatch, cap enforcement.

These tests exist because as of the threading work the agent is no longer
single-client. Two connections must be able to run commands without one
blocking the other; the (N+1)th connection past `max_connections` must be
refused with `ERR busy` and not crash the agent.
"""
import threading
import time

from client import AgentClient, AgentError
from fixtures import ConformanceTestCase


class ConcurrencyTests(ConformanceTestCase):
    def test_two_connections_dont_block_each_other(self):
        """One connection runs SLEEP 2000; the other must answer PING
        immediately, not after the sleep."""
        slow = AgentClient(self.host, self.port, timeout=10).connect()
        fast = AgentClient(self.host, self.port, timeout=10).connect()
        try:
            holder = {}

            def slow_call():
                holder["slow_start"] = time.time()
                slow.cmd_ok("SLEEP 2000")
                holder["slow_end"] = time.time()

            t = threading.Thread(target=slow_call)
            t.start()
            time.sleep(0.2)  # let SLEEP start

            t0 = time.time()
            self.assertEqual(fast.cmd("PING"), "OK pong")
            ping_elapsed = time.time() - t0

            t.join(timeout=5)
            self.assertFalse(t.is_alive(), "SLEEP thread did not finish in time")

            self.assertLess(
                ping_elapsed, 0.5,
                f"PING on connection B took {ping_elapsed:.2f}s while A was "
                f"in SLEEP — connections are still serialised",
            )
        finally:
            slow.disconnect()
            fast.disconnect()

    def test_max_connections_advertised(self):
        info = self.info()
        self.assertIn("max_connections", info,
                      "INFO must include max_connections")
        n = int(info["max_connections"])
        self.assertGreaterEqual(n, 1)
        self.assertLessEqual(n, 64,
                             "max_connections seems implausibly high")

    def test_excess_connections_refused_with_busy(self):
        """Open max_connections clients, then try to open one more — the
        excess must receive ERR busy or fail to negotiate cleanly."""
        info = self.info()
        n = int(info.get("max_connections", "0"))
        if n == 0 or n > 16:
            self.skipTest(f"max_connections={n} — skipping flood test")

        held = []
        try:
            for _ in range(n):
                c = AgentClient(self.host, self.port, timeout=5).connect()
                # Verify the connection is real
                self.assertEqual(c.cmd("PING"), "OK pong")
                held.append(c)

            # The (n+1)th must be rejected. Either we get ERR busy on the
            # very first read, or the connect itself fails cleanly.
            try:
                extra = AgentClient(self.host, self.port, timeout=5).connect()
                line = extra.cmd("PING")
                self.assertTrue(
                    line.startswith("ERR"),
                    f"agent at cap should refuse with ERR; got: {line!r}",
                )
                extra.disconnect()
            except (OSError, AgentError):
                pass  # connect refused; also acceptable

            # Now release one slot and retry — should succeed.
            held.pop().disconnect()
            time.sleep(0.5)  # let the worker thread tear down
            recovered = AgentClient(self.host, self.port, timeout=5).connect()
            try:
                self.assertEqual(recovered.cmd("PING"), "OK pong")
            finally:
                recovered.disconnect()
        finally:
            for c in held:
                c.disconnect()

    def test_parallel_input_during_long_running_op(self):
        """The motivating use case: one connection runs a long command
        (here SLEEP, eventually WATCH), another sends MOVE/MPOS in parallel."""
        self.require_caps("MOVE", "MPOS", "SCREEN")
        self.require_desktop_session()
        long_op = AgentClient(self.host, self.port, timeout=10).connect()
        cmd_op = AgentClient(self.host, self.port, timeout=10).connect()
        try:
            scr = cmd_op.cmd_ok("SCREEN").split()
            screen_w = int(scr[1])
            screen_h = int(scr[2])
            target_x = min(150, screen_w // 2)
            target_y = min(150, screen_h // 2)

            done = threading.Event()

            def sleeper():
                long_op.cmd_ok("SLEEP 1500")
                done.set()

            t = threading.Thread(target=sleeper)
            t.start()
            time.sleep(0.1)

            # Drive input on the other connection while the sleep is in flight
            cmd_op.cmd_ok(f"MOVE {target_x} {target_y}")
            line = cmd_op.cmd_ok("MPOS")
            parts = line.split()
            self.assertLess(abs(int(parts[1]) - target_x), 10)
            self.assertLess(abs(int(parts[2]) - target_y), 10)

            # Make sure the long op is still running, not blocked
            self.assertFalse(done.is_set(),
                             "SLEEP completed too early — was it blocked?")

            t.join(timeout=5)
            self.assertTrue(done.is_set(), "SLEEP never finished")
        finally:
            long_op.disconnect()
            cmd_op.disconnect()

    def test_two_connections_independent_proc_tables(self):
        """Each connection sees the same process table — EXEC on one,
        WAIT on another, must work."""
        self.require_caps("EXEC", "WAIT")
        is_windows = self.info().get("os", "").startswith("windows")
        if not is_windows:
            self.skipTest("only verifying the windows path")

        c1 = AgentClient(self.host, self.port, timeout=10).connect()
        c2 = AgentClient(self.host, self.port, timeout=10).connect()
        try:
            line = c1.cmd_ok("EXEC cmd /c exit 0")
            pid = int(line.split()[1])
            wait_line = c2.cmd_ok(f"WAIT {pid} 5000")
            self.assertEqual(wait_line.split()[1], "0")
        finally:
            c1.disconnect()
            c2.disconnect()
