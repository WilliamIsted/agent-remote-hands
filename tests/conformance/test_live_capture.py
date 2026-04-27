"""WATCH, WAITFOR, ABORT — long-running capture and cancellation."""
import struct
import threading
import time

from client import AgentClient, AgentError
from fixtures import ConformanceTestCase


def _read_frame(sock_client):
    """Read one OK <ts> <len>\\n<bytes> frame from a multi-frame stream.
    Returns (ts_ms, data_bytes) or (None, None) on END or error."""
    line = sock_client._recv_line()
    if line == "END":
        return None, None
    if line.startswith("ERR"):
        return None, line
    if not line.startswith("OK "):
        raise AssertionError(f"unexpected frame line: {line!r}")
    parts = line.split()
    if len(parts) < 3:
        raise AssertionError(f"WATCH frame must be 'OK <ts> <len>': {line!r}")
    ts = int(parts[1])
    length = int(parts[-1])
    data = sock_client._recv_n(length)
    return ts, data


class WatchTests(ConformanceTestCase):
    def test_watch_emits_baseline_and_end(self):
        """WATCH always sends a baseline frame, then END after duration."""
        self.require_caps("WATCH")
        with self.connect() as c:
            c._send(b"WATCH 200 1500\n")
            ts, data = _read_frame(c)
            self.assertIsNotNone(ts, "WATCH should always emit a baseline frame")
            self.assertEqual(data[:2], b"BM", "frame should be a BMP")

            # Drain remaining frames until END or timeout
            saw_end = False
            deadline = time.time() + 5
            while time.time() < deadline:
                ts2, data2 = _read_frame(c)
                if ts2 is None and data2 is None:
                    saw_end = True
                    break
                if isinstance(data2, str) and data2.startswith("ERR"):
                    self.fail(f"WATCH errored mid-stream: {data2}")
            self.assertTrue(saw_end, "WATCH did not terminate with END within duration")

    def test_watch_invalid_args(self):
        self.require_caps("WATCH")
        with self.connect() as c:
            self.assertTrue(c.cmd("WATCH").startswith("ERR"))
            self.assertTrue(c.cmd("WATCH 100").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")


class WaitForTests(ConformanceTestCase):
    def test_waitfor_timeout_when_idle(self):
        """No screen change in 1s with high threshold → ERR timeout."""
        self.require_caps("WAITFOR")
        with self.connect() as c:
            # threshold 99% — needs basically the whole screen to change
            line = c.cmd("WAITFOR 1000 99.0")
            self.assertTrue(line.startswith("ERR"),
                            f"expected ERR (timeout), got: {line!r}")

    def test_waitfor_invalid_args(self):
        self.require_caps("WAITFOR")
        with self.connect() as c:
            self.assertTrue(c.cmd("WAITFOR").startswith("ERR"))
            self.assertTrue(c.cmd("WAITFOR -1").startswith("ERR"))
            self.assertEqual(c.cmd("PING"), "OK pong")


class AbortTests(ConformanceTestCase):
    def test_abort_cancels_sleep(self):
        """One connection runs SLEEP 5000; another sends ABORT; the SLEEP
        returns ERR aborted within ~200 ms."""
        self.require_caps("ABORT", "SLEEP")
        slow = AgentClient(self.host, self.port, timeout=10).connect()
        ctrl = AgentClient(self.host, self.port, timeout=10).connect()
        try:
            holder = {}

            def long_sleep():
                holder["start"] = time.time()
                holder["resp"] = slow.cmd("SLEEP 5000")
                holder["end"] = time.time()

            t = threading.Thread(target=long_sleep)
            t.start()
            time.sleep(0.3)  # let SLEEP start

            self.assertEqual(ctrl.cmd("ABORT"), "OK")
            t.join(timeout=2)
            self.assertFalse(t.is_alive(), "SLEEP didn't return after ABORT")
            self.assertTrue(holder["resp"].startswith("ERR"),
                            f"SLEEP after ABORT: {holder['resp']!r}")
            self.assertIn("aborted", holder["resp"].lower())
            self.assertLess(holder["end"] - holder["start"], 1.0,
                            "ABORT didn't cancel SLEEP within 1s")
        finally:
            slow.disconnect()
            ctrl.disconnect()

    def test_abort_cancels_watch(self):
        """WATCH on connection A, ABORT on connection B, A's WATCH ends."""
        self.require_caps("ABORT", "WATCH")
        watcher = AgentClient(self.host, self.port, timeout=10).connect()
        ctrl = AgentClient(self.host, self.port, timeout=10).connect()
        try:
            watcher._send(b"WATCH 200 30000\n")
            # Read the baseline frame
            ts, data = _read_frame(watcher)
            self.assertIsNotNone(ts)
            time.sleep(0.3)

            self.assertEqual(ctrl.cmd("ABORT"), "OK")

            # The WATCH should now emit either END or ERR aborted soon
            t0 = time.time()
            terminated = False
            while time.time() - t0 < 2.0:
                line = watcher._recv_line()
                if line == "END" or line.startswith("ERR"):
                    terminated = True
                    break
                if line.startswith("OK "):
                    # Read past the next frame and continue
                    parts = line.split()
                    n = int(parts[-1])
                    watcher._recv_n(n)
            self.assertTrue(terminated,
                            "WATCH didn't terminate within 2s of ABORT")
        finally:
            watcher.disconnect()
            ctrl.disconnect()

    def test_abort_does_not_disconnect_sender(self):
        """ABORT keeps the sender's connection open."""
        self.require_caps("ABORT")
        with self.connect() as c:
            self.assertEqual(c.cmd("ABORT"), "OK")
            self.assertEqual(c.cmd("PING"), "OK pong")

    def test_abort_only_affects_in_flight(self):
        """A future SLEEP after an ABORT runs to completion — only verbs
        that were already running when ABORT arrived should be cancelled."""
        self.require_caps("ABORT", "SLEEP")
        with self.connect() as c:
            self.assertEqual(c.cmd("ABORT"), "OK")
            t0 = time.time()
            line = c.cmd("SLEEP 200")
            elapsed = time.time() - t0
            self.assertTrue(line.startswith("OK"),
                            f"new SLEEP shouldn't be aborted: {line}")
            self.assertGreaterEqual(elapsed, 0.15)
