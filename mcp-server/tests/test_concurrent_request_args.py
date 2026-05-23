#   Copyright 2026 William Isted and contributors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

"""Regression test for the wire-framing race fixed in commit ``c7d01c2``.

Background
----------

Session 2 of the v0.3.0 test campaign (BM004) wedged the MCP bridge on its
first batch of parallel tool calls. Three concurrent forwarded verbs all
returned ``WireError("MCP frame missing Content-Length")``; every subsequent
forwarded call thereafter failed identically. Root cause: ``AgentClient``
shared a single socket and single ``_buf`` bytearray across
``asyncio.to_thread`` worker threads, with no mutual exclusion around the
send-then-receive pair. Two threads racing through ``request_args``
interleaved bytes in the receive buffer; length-prefixed framing has no
resync token, so the byte offset was permanently lost.

Commit ``c7d01c2`` wrapped the public methods (``request`` / ``request_args``
/ ``info`` / ``capabilities`` / ``close`` / ``wait_for_event``) in
``with self._lock:`` (an ``RLock``). This file proves the fix has the
property it claims, and demonstrates that property is *necessary* by
showing the same scenario fail when the lock is removed.

Test design — two complementary properties
-------------------------------------------

The lock's actual semantics are stronger than "concurrent calls succeed":
it serialises *all* forwarded calls into one-at-a-time wire access. So
"concurrent calls succeed" is a vacuous integration test under the lock —
calls never overlap at the wire to begin with. What we want to prove is:

1. **The lock provides mutual exclusion** (forward direction). Stub the
   bridge's transport methods to count the number of threads simultaneously
   between ``_send_mcp`` and the matching ``_read_mcp_response``. Fire N
   threads. Assert the count never exceeds 1. Deterministic; tests the
   lock invariant directly.

2. **The lock is necessary** (reverse direction / regression demonstration).
   Monkey-patch ``AgentClient.request_args`` to bypass the lock. The mock
   uses a response barrier to widen the race window deterministically.
   Fire N threads. Assert at least one observable failure — exception, a
   crossed response (rid-to-body mismatch), or a missing reply.

Without test (2), test (1) could pass on a no-op assertion. Without
test (1), the lock could be present-but-broken (e.g. a non-reentrant Lock
deadlocking on the reentrant path through ``connect``) and only test (1)
would catch it. Both matter.
"""

from __future__ import annotations

import contextlib
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List

import pytest

from agent_client import AgentClient, OkResponse


# Number of concurrent caller threads. Picked to exceed any plausible
# "happen-to-be-serial" coincidence while staying within the mock's listen
# backlog (4 — bumped to 8 in the barrier fixture) and the barrier wait
# budget (10s).
CONCURRENCY = 6


# ----------------------------------------------------------------------
# Mock setup — a verb that echoes a caller-supplied nonce back. The nonce
# lets reverse-direction assertions check rid-to-body correspondence.

def _register_echo_verb(mock_agent) -> None:
    def handler(arguments, payload):
        return ("ok", {
            "nonce": arguments.get("nonce", ""),
            "thread": threading.get_ident(),
        })
    mock_agent.register("test.echo_nonce", handler)


# ----------------------------------------------------------------------
# Fixtures

@pytest.fixture
def plain_mock_agent():
    """Mock without a response barrier — used by the forward (lock-holds)
    test, which exercises the lock invariant directly and doesn't need the
    wire-level race window widened."""
    from mock_agent import MockAgent
    agent = MockAgent()
    agent.start()
    _register_echo_verb(agent)
    try:
        yield agent
    finally:
        agent.stop()


@pytest.fixture
def barrier_mock_agent():
    """Mock with a response barrier sized for ``CONCURRENCY`` callers —
    used by the reverse (regression) test. Each ``test.echo_nonce``
    dispatch parks until all ``CONCURRENCY`` requests have arrived, then
    all responses fire simultaneously — guaranteeing every caller thread
    is in ``recv()`` together when the unlocked race window opens."""
    from mock_agent import MockAgent
    agent = MockAgent(release_barrier_n=CONCURRENCY)
    agent.start()
    _register_echo_verb(agent)
    try:
        yield agent
    finally:
        agent.stop()


def _connected(mock_agent) -> AgentClient:
    c = AgentClient("127.0.0.1", mock_agent.port, timeout=15.0)
    c.connect()
    return c


# ----------------------------------------------------------------------
# Shared helper

def fire_concurrent_calls(client: AgentClient, n: int) -> List:
    """Submit ``n`` concurrent ``request_args`` calls to ``test.echo_nonce``,
    each with a unique nonce. Returns ``[(nonce, result_or_exc), ...]`` in
    submission order. Exceptions are returned, not raised, so assertions can
    inspect them."""
    nonces = [f"n{i:02d}" for i in range(n)]
    results: List = [None] * n

    def call_one(i: int):
        try:
            r = client.request_args("test.echo_nonce", {"nonce": nonces[i]})
            return (nonces[i], r)
        except Exception as exc:  # noqa: BLE001
            return (nonces[i], exc)

    with ThreadPoolExecutor(max_workers=n) as pool:
        futures = [pool.submit(call_one, i) for i in range(n)]
        for f in as_completed(futures):
            pair = f.result()
            results[nonces.index(pair[0])] = pair
    return results


# ======================================================================
# (1) Forward proof — the RLock provides mutual exclusion.
#
# Stubs ``_send_mcp`` and ``_read_mcp_response`` to count threads inside
# the critical section. With the RLock, the count is bounded at 1 even
# under fire-N-threads pressure. Doesn't go through the mock socket; tests
# the lock invariant directly, not its consequences.
# ======================================================================

class ConcurrentEntrantTracker:
    """Wraps the bridge's transport methods to record concurrent entrants
    into the ``request_args`` critical section. ``max_concurrent`` is the
    high-water mark of threads simultaneously between ``_send_mcp`` and
    ``_read_mcp_response`` during the test run."""

    def __init__(self) -> None:
        self._inside = 0
        self._max = 0
        self._guard = threading.Lock()  # protects _inside / _max only

    def enter(self) -> None:
        with self._guard:
            self._inside += 1
            self._max = max(self._max, self._inside)

    def leave(self) -> None:
        with self._guard:
            self._inside -= 1

    @property
    def max_concurrent(self) -> int:
        return self._max


@contextlib.contextmanager
def _patched_transport(client: AgentClient, tracker: ConcurrentEntrantTracker,
                       hold_ms: float = 25.0):
    """Replace ``client``'s ``_send_mcp`` and ``_read_mcp_response`` with
    instrumented versions that bracket the critical section and add a small
    hold time inside the section to make concurrent-entry trivially visible
    if the lock is ever absent. Restores on exit."""
    import time

    orig_send = client._send_mcp
    orig_read = client._read_mcp_response

    def send_then_track(obj):
        tracker.enter()
        # Stay inside the critical section briefly so any concurrent thread
        # that bypassed the lock would visibly overlap us in `tracker`.
        time.sleep(hold_ms / 1000.0)
        return orig_send(obj)

    def read_then_track(rid):
        try:
            return orig_read(rid)
        finally:
            tracker.leave()

    client._send_mcp = send_then_track  # type: ignore[method-assign]
    client._read_mcp_response = read_then_track  # type: ignore[method-assign]
    try:
        yield
    finally:
        client._send_mcp = orig_send  # type: ignore[method-assign]
        client._read_mcp_response = orig_read  # type: ignore[method-assign]


def test_lock_provides_mutual_exclusion(plain_mock_agent) -> None:
    """Under the c7d01c2 RLock, no two threads are ever simultaneously
    between ``_send_mcp`` and the matching ``_read_mcp_response`` — the
    critical section is mutually exclusive.

    TODO(user): write the assertion. ``tracker.max_concurrent`` records the
    high-water mark of concurrent entrants observed during the run. The
    forward property is:

        max_concurrent == 1     →   strict mutual exclusion held
        max_concurrent  > 1     →   the lock was bypassed for at least one
                                    overlap and the property is broken

    Also assert every caller got a successful, correctly-bodied response
    — a passing exclusion property paired with cross-routed responses
    would still be a bug. The ``results`` list pairs nonces with
    OkResponse/Exception; ``r.json()["nonce"]`` recovers the body's nonce.

    ~5-10 lines.
    """
    client = _connected(plain_mock_agent)
    tracker = ConcurrentEntrantTracker()
    try:
        with _patched_transport(client, tracker):
            results = fire_concurrent_calls(client, CONCURRENCY)
    finally:
        client.close()

    # Sanity: every call returned (no missing entries from the helper).
    assert len(results) == CONCURRENCY

    # Mutual exclusion: exactly one thread was ever between _send_mcp and
    # the matching _read_mcp_response. Strict equality (rather than <= 1)
    # also catches the case where the instrumentation never fired — a
    # max_concurrent of 0 would mean the patched transport was bypassed.
    assert tracker.max_concurrent == 1, (
        f"lock failed mutual exclusion: peak concurrent entrants = "
        f"{tracker.max_concurrent}")

    # Response correctness: every caller got back the body whose nonce
    # matches the one it submitted. The mutual-exclusion property above
    # is necessary but not sufficient — a buggy rid demultiplex could
    # serialise calls correctly yet still cross responses on the way back.
    for nonce, result in results:
        assert isinstance(result, OkResponse), (
            f"thread for {nonce} raised "
            f"{type(result).__name__}: {result!r}")
        assert result.json()["nonce"] == nonce, (
            f"crossed response: submitted {nonce!r}, "
            f"got {result.json().get('nonce')!r}")


# ======================================================================
# (2) Reverse demonstration — the RLock is necessary.
#
# Bypasses the lock via monkey-patch; uses the barrier-equipped mock to
# guarantee all N caller threads are in recv() together when responses
# arrive. Asserts observable framing damage.
# ======================================================================

@contextlib.contextmanager
def _request_args_without_lock():
    """Replace ``AgentClient.request_args`` with one that skips the lock and
    calls the parent ``WireClient.request_args`` directly. Restores on exit.
    Class-level patch so all instances see the unlocked path."""
    from agent_client import AgentClient as AC

    original = AC.request_args

    def unlocked(self, verb, arguments):
        return super(AC, self).request_args(verb, arguments)

    AC.request_args = unlocked
    try:
        yield
    finally:
        AC.request_args = original


def test_lock_is_necessary(barrier_mock_agent) -> None:
    """With the RLock monkey-patched out, ``CONCURRENCY`` concurrent calls
    through one ``AgentClient`` produce observable damage — at least one
    exception, a crossed response, or a missing reply. If this test ever
    starts passing (no damage on a given run), the forward proof in (1)
    is no longer falsifiable and one of: (a) the race has been closed at
    a different layer, (b) the OS scheduling is happening to serialise the
    threads, (c) the stress strategy needs widening. The first is good news;
    the others mean the test is now unreliable.

    TODO(user): write the assertion. The contract is the contrapositive of
    (1):

        Without the lock, at least one of these must hold:
        - some ``results[i][1]`` is an Exception (most likely WireError)
        - some OkResponse body's ``nonce`` differs from the submitted nonce
          for that pair (crossed response — rid-routing bug)
        - len({pair[0] for pair in results}) < CONCURRENCY (missing reply
          or hung thread)

    Note: this test is *expected to be flaky on the failure side* because
    the race is non-deterministic. The barrier biases the timing strongly
    toward firing it but cannot guarantee it. If flakiness becomes a real
    problem, options are: bump CONCURRENCY, repeat the test N times in a
    loop and require failure at least once, or accept high-but-imperfect
    confidence as the cost of demonstrating a real concurrency property.

    ~5-10 lines.
    """
    client = _connected(barrier_mock_agent)
    try:
        with _request_args_without_lock():
            results = fire_concurrent_calls(client, CONCURRENCY)
    finally:
        client.close()

    # Three failure modes, any one of which proves the property: an
    # exception raised in any caller, a crossed nonce on any OkResponse,
    # or a missing reply (some thread's reply never made it back).
    exceptions = [(n, r) for n, r in results
                  if isinstance(r, Exception)]
    crossed = [(n, r) for n, r in results
               if isinstance(r, OkResponse)
               and r.json().get("nonce") != n]
    matched_nonces = {n for n, r in results
                      if isinstance(r, OkResponse)
                      and r.json().get("nonce") == n}
    missing = CONCURRENCY - len(matched_nonces)

    assert exceptions or crossed or missing > 0, (
        "lock-removed scenario produced no observable damage — the test "
        "is no longer falsifying the c7d01c2 fix. Investigate: has the "
        "race been closed at a different layer, or has stress widening "
        "(CONCURRENCY, payload size) become insufficient?")
