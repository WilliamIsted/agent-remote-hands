"""Base TestCase used by every conformance module.

The conformance contract is "if a verb is in CAPS, it must work". Tests use
`require_caps()` to skip cleanly on agents that don't advertise a verb,
rather than hard-failing — the same suite then grades windows-nt,
windows-9x, windows-modern, etc. against the subset each implements.
"""
import os
import unittest
import uuid

from client import AgentClient, AgentError


class ConformanceTestCase(unittest.TestCase):
    host = None
    port = 8765
    timeout = 30

    _caps_cache = None
    _info_cache = None

    def connect(self):
        return AgentClient(self.host, self.port, self.timeout)

    @classmethod
    def _query_caps(cls):
        if cls._caps_cache is None:
            with AgentClient(cls.host, cls.port, cls.timeout) as c:
                line = c.cmd("CAPS")
            cls._caps_cache = set(line.split()[1:]) if line.startswith("OK") else set()
        return cls._caps_cache

    @classmethod
    def _query_info(cls):
        if cls._info_cache is None:
            with AgentClient(cls.host, cls.port, cls.timeout) as c:
                line = c.cmd("INFO")
            kv = {}
            if line.startswith("OK"):
                for tok in line.split()[1:]:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        kv[k] = v
            cls._info_cache = kv
        return cls._info_cache

    def caps(self):
        return self._query_caps()

    def info(self):
        return self._query_info()

    def require_caps(self, *verbs):
        missing = [v for v in verbs if v not in self.caps()]
        if missing:
            self.skipTest(f"agent lacks: {', '.join(missing)}")

    def require_info(self, key, value=None):
        info = self.info()
        if key not in info:
            self.skipTest(f"INFO missing key: {key}")
        if value is not None and info[key] != value:
            self.skipTest(f"INFO[{key}]={info[key]!r}, need {value!r}")

    def require_desktop_session(self):
        """Skip the test if the agent is running without an interactive
        desktop session (headless service, WinRM, etc.). Detected by
        probing MPOS — the agent ERRs with "no desktop session" when
        GetCursorPos can't reach a real desktop. Cached per-class to
        avoid hammering the agent."""
        cls = type(self)
        cached = getattr(cls, "_desktop_cache", None)
        if cached is None:
            cached = True
            try:
                with self.connect() as c:
                    line = c.cmd("MPOS")
                if line.startswith("ERR") and "no desktop session" in line:
                    cached = False
            except (AgentError, OSError):
                pass
            cls._desktop_cache = cached
        if not cached:
            self.skipTest("agent has no interactive desktop session")

    def temp_dir(self):
        """A writable directory on the target. Uses the agent's TEMP env var
        if available, otherwise falls back per OS family."""
        if "ENV" in self.caps():
            try:
                with self.connect() as c:
                    line = c.cmd("ENV TEMP")
                if line.startswith("OK "):
                    return line[3:].strip()
            except (AgentError, OSError):
                pass
        os_name = self.info().get("os", "")
        if os_name.startswith("windows"):
            return "C:\\Temp"
        return "/tmp"

    def workspace(self):
        """A unique per-test subdirectory under temp_dir(), created on demand
        and torn down by the cleanup hook. Returns the full path."""
        base = self.temp_dir()
        sep = "\\" if base[1:2] == ":" else "/"
        path = f"{base}{sep}rh-conf-{uuid.uuid4().hex[:8]}"
        with self.connect() as c:
            try:
                c.cmd_ok(f"MKDIR {path}")
            except AgentError as e:
                self.skipTest(f"cannot create workspace {path}: {e}")
        self.addCleanup(self._rm_workspace, path)
        return path

    def _rm_workspace(self, path):
        """Best-effort cleanup. Walks the directory via LIST and DELETEs each
        entry, then removes the directory itself."""
        try:
            with self.connect() as c:
                if "LIST" in self.caps():
                    try:
                        _, data = c.cmd_data(f"LIST {path}")
                        for row in data.decode("latin-1").splitlines():
                            fields = row.split("\t")
                            if len(fields) >= 4:
                                name = fields[3]
                                sep = "\\" if path[1:2] == ":" else "/"
                                try:
                                    c.cmd_ok(f"DELETE {path}{sep}{name}")
                                except AgentError:
                                    pass
                    except AgentError:
                        pass
                try:
                    c.cmd_ok(f"DELETE {path}")
                except AgentError:
                    pass
        except OSError:
            pass

    def path_join(self, *parts):
        """Path join using the target OS's separator (read from INFO)."""
        os_name = self.info().get("os", "")
        sep = "\\" if os_name.startswith("windows") else "/"
        return sep.join(parts)
