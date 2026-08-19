#!/usr/bin/env python3
"""Self-test: drives pump_client.py against stub_server.py.

    python3 test_examples.py

Proves that the reference client and the stub agree with each other and with
the firmware's documented contract. Run this after changing either file.

Standard library only; no hardware, no network beyond loopback.
"""

from __future__ import annotations

import os
import socket
import sys
import time
import urllib.error
import urllib.request

# Runnable from any working directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pump_client import (PumpAuthError, PumpClient, PumpClientBug,
                         PumpConflict, PumpUnreachable, StatusPoller,
                         new_request_id)
from stub_server import (CHOKE_PREP_MS, CRANK_DURATION_MS, KILL_HOLD_MS,
                         UNCHOKE_DELAY_MS, VALVE_CLOSE_DELAY_MS,
                         VALVE_PRIME_MS, serve)

FULL_START_S = (VALVE_PRIME_MS + CHOKE_PREP_MS + CRANK_DURATION_MS +
                UNCHOKE_DELAY_MS) / 1000
FULL_STOP_S = (KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS) / 1000

SECRET = "test-secret-abcdefghijklmnop"

_passed = 0
_failed = 0


def check(name: str, condition: bool, detail: str = "") -> bool:
    global _passed, _failed
    if condition:
        _passed += 1
        print(f"  ok   {name}" + (f"  ({detail})" if detail else ""))
    else:
        _failed += 1
        print(f"  FAIL {name}" + (f"  ({detail})" if detail else ""))
    return condition


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def settle_to_idle(pump: PumpClient) -> None:
    """Reach IDLE with the recrank cooldown expired."""
    st = pump.status()
    if st.state != "IDLE":
        pump.stop()
        time.sleep(FULL_STOP_S + 0.3)
    remaining = pump.status().cooldown_remaining_ms
    if remaining:
        time.sleep(remaining / 1000 + 0.2)
    for _ in range(100):
        if pump.status().state == "IDLE":
            return
        time.sleep(0.05)
    raise AssertionError(f"could not reach IDLE (stuck in {pump.status().state})")


def main() -> int:
    port = free_port()
    _httpd, _model, shutdown = serve(port, SECRET)
    time.sleep(0.2)

    pump = PumpClient("127.0.0.1", SECRET, port=port, timeout=2.0)

    try:
        # -- boot state ----------------------------------------------------
        print("\n[boot]")
        st = pump.status()
        check("initial state is UNKNOWN", st.state == "UNKNOWN", st.state)
        check("running_confirmed is false", st.running_confirmed is False)
        # The kill is ASSERTED at rest -- that is the fail-safe position, not
        # an idle relay. Only the three that can do work must be off.
        check("starter, choke and intake are all at rest",
              not st.relay_outputs.starter and not st.relay_outputs.choke
              and not st.relay_outputs.valve, str(st.relay_outputs))
        check("the engine is inhibited at boot (kill asserted)",
              st.relay_outputs.kill is True)
        check("water interlock reports available", st.water_ok is True)
        check("needs_operator_reset is true", st.needs_operator_reset)
        check("start_permitted is false", not st.start_permitted)
        check("firmware version reported", st.firmware_version == "0.2.0",
              st.firmware_version)

        # -- START refused from UNKNOWN ------------------------------------
        print("\n[START gating]")
        try:
            pump.start()
            check("START from UNKNOWN raises PumpConflict", False, "no exception")
        except PumpConflict as exc:
            check("START from UNKNOWN raises PumpConflict", True,
                  f"state={exc.state}")

        # -- reset-idle ----------------------------------------------------
        print("\n[reset-idle]")
        r = pump.reset_idle()
        check("reset-idle accepted", r.accepted)
        check("reset-idle reaches IDLE", r.state == "IDLE", r.state)
        check("start_permitted becomes true", pump.status().start_permitted)

        # -- full start sequence -------------------------------------------
        print("\n[start sequence]")
        r = pump.start()
        check("START accepted from IDLE", r.accepted)
        check("START enters PRIMING (intake first, not the choke)",
              r.state == "PRIMING", r.state)
        check("START opened the intake", pump.status().relay_outputs.valve)
        check("START did NOT release the kill yet",
              pump.status().relay_outputs.kill is True)
        check("START is not a duplicate", not r.duplicate)

        seen = set()
        deadline = time.monotonic() + FULL_START_S + 4
        while time.monotonic() < deadline:
            s = pump.status()
            seen.add(s.state)
            if s.state == "RUNNING_ASSUMED":
                break
            time.sleep(0.05)

        for expected in ("PRIMING", "CHOKING", "CRANKING", "UNCHOKING",
                         "RUNNING_ASSUMED"):
            check(f"observed {expected}", expected in seen, str(sorted(seen)))

        st = pump.status()
        check("running_confirmed still false in RUNNING_ASSUMED",
              st.running_confirmed is False)
        check("display text does not claim 'running'",
              "running" not in st.display_text.lower(), st.display_text)
        check("display text points at the camera",
              "camera" in st.display_text.lower(), st.display_text)
        check("cooldown is now outstanding", st.cooldown_remaining_ms > 0,
              f"{st.cooldown_remaining_ms} ms")
        check("the intake stays open while running", st.relay_outputs.valve)
        check("the engine is permitted to run", st.relay_outputs.kill is False)

        # -- START refused mid-sequence and during cooldown ----------------
        print("\n[START refused outside IDLE]")
        try:
            pump.start()
            check("START from RUNNING_ASSUMED raises PumpConflict", False)
        except PumpConflict as exc:
            check("START from RUNNING_ASSUMED raises PumpConflict", True,
                  f"state={exc.state}")

        # -- STOP from RUNNING_ASSUMED -------------------------------------
        print("\n[STOP]")
        r = pump.stop()
        check("STOP accepted from RUNNING_ASSUMED", r.accepted)
        check("STOP enters STOPPING", r.state == "STOPPING", r.state)
        check("kill grounded", pump.status().relay_outputs.kill)
        check("starter released", not pump.status().relay_outputs.starter)
        check("STOP leaves the intake OPEN (never shut on a running pump)",
              pump.status().relay_outputs.valve)

        time.sleep(KILL_HOLD_MS / 1000 + 0.3)
        st = pump.status()
        check("enters VALVE_CLOSING after the kill hold",
              st.state == "VALVE_CLOSING", st.state)
        check("intake still open in VALVE_CLOSING", st.relay_outputs.valve)

        time.sleep(VALVE_CLOSE_DELAY_MS / 1000 + 0.4)
        st = pump.status()
        check("returns to IDLE after the intake closes", st.state == "IDLE", st.state)
        check("intake shut", not st.relay_outputs.valve)
        check("the kill stays asserted at rest", st.relay_outputs.kill is True)

        # -- START refused during the recrank cooldown ---------------------
        st = pump.status()
        if st.cooldown_remaining_ms > 0:
            try:
                pump.start()
                check("START during cooldown raises PumpConflict", False)
            except PumpConflict as exc:
                check("START during cooldown raises PumpConflict", True,
                      f"{exc.cooldown_remaining_ms} ms left")
                check("conflict carries a positive cooldown",
                      exc.cooldown_remaining_ms > 0)
        else:
            check("cooldown was observable", False, "cooldown already expired")

        # -- idempotency ----------------------------------------------------
        print("\n[idempotency]")
        settle_to_idle(pump)
        rid = new_request_id("start")
        a = pump.start(request_id=rid)
        b = pump.start(request_id=rid)
        c = pump.start(request_id=rid)
        check("first START not flagged duplicate", not a.duplicate)
        check("replayed START flagged duplicate", b.duplicate)
        check("second replay flagged duplicate", c.duplicate)
        check("replay does not restart the sequence", b.accepted and c.accepted)

        # Wait out the sequence, then confirm only one crank happened by
        # checking we are in RUNNING_ASSUMED and not cranking again.
        time.sleep(FULL_START_S + 0.6)
        check("sequence completed once", pump.status().state == "RUNNING_ASSUMED",
              pump.status().state)

        # -- STOP is exempt from duplicate suppression ---------------------
        print("\n[STOP duplicate exemption]")
        srid = new_request_id("stop")
        s1 = pump.stop(request_id=srid)
        check("first STOP not flagged duplicate", not s1.duplicate)
        time.sleep(FULL_STOP_S + 0.4)
        check("returned to IDLE", pump.status().state == "IDLE")

        s2 = pump.stop(request_id=srid)
        check("replayed STOP is flagged duplicate", s2.duplicate)
        check("replayed STOP is STILL executed", s2.state == "STOPPING", s2.state)
        check("replayed STOP re-grounds the kill circuit",
              pump.status().relay_outputs.kill)
        time.sleep(FULL_STOP_S + 0.4)

        # -- start-failed ---------------------------------------------------
        print("\n[start-failed]")
        settle_to_idle(pump)
        pump.start()
        time.sleep(FULL_START_S + 0.6)
        check("reached RUNNING_ASSUMED", pump.status().state == "RUNNING_ASSUMED")

        r = pump.start_failed()
        check("start-failed accepted", r.accepted)
        check("start-failed enters RETRY_WAIT", r.state == "RETRY_WAIT", r.state)
        st = pump.status()
        check("starter, choke and intake all off in RETRY_WAIT",
              not st.relay_outputs.starter and not st.relay_outputs.choke
              and not st.relay_outputs.valve, str(st.relay_outputs))
        check("start-failed shuts the intake (engine never caught)",
              not st.relay_outputs.valve)
        check("start-failed leaves the engine inhibited",
              st.relay_outputs.kill is True)
        check("RETRY_WAIT reports a cooldown", st.cooldown_remaining_ms > 0)

        # It must never auto-retry.
        crank_seen = False
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if pump.status().relay_outputs.starter:
                crank_seen = True
                break
            time.sleep(0.05)
        check("never auto-retries from RETRY_WAIT", not crank_seen)

        # -- authentication -------------------------------------------------
        print("\n[authentication]")
        bad = PumpClient("127.0.0.1", "wrong-secret", port=port, timeout=2.0)
        try:
            bad.status()
            check("wrong secret raises PumpAuthError", False)
        except PumpAuthError:
            check("wrong secret raises PumpAuthError", True)

        try:
            bad.start()
            check("wrong secret cannot start the pump", False)
        except PumpAuthError:
            check("wrong secret cannot start the pump", True)
        except PumpConflict:
            check("wrong secret cannot start the pump", False,
                  "got 409 -- auth is being checked after routing")

        # 401 must come before routing, so unknown paths also answer 401.
        req = urllib.request.Request(
            f"http://127.0.0.1:{port}/v1/definitely-not-real",
            headers={"X-Pump-Secret": "wrong"})
        try:
            urllib.request.urlopen(req, timeout=2)
            check("unauthenticated unknown path returns 401", False, "got 2xx")
        except urllib.error.HTTPError as exc:
            check("unauthenticated unknown path returns 401", exc.code == 401,
                  f"HTTP {exc.code}")

        # Authenticated unknown path is a 404 (client bug).
        try:
            pump._request("GET", "/v1/definitely-not-real")
            check("authenticated unknown path raises PumpClientBug", False)
        except PumpClientBug:
            check("authenticated unknown path raises PumpClientBug", True)

        # Wrong method on a known path.
        try:
            pump._request("GET", "/v1/start")
            check("GET on /v1/start raises PumpClientBug", False)
        except PumpClientBug:
            check("GET on /v1/start raises PumpClientBug", True)

        # -- request id validation ------------------------------------------
        print("\n[request ids]")
        for bad_id in ("has space", "has/slash", "dot.dot", "a" * 65, ""):
            try:
                pump.stop(request_id=bad_id)
                check(f"rejects invalid request id {bad_id!r}", False)
            except (ValueError, PumpClientBug):
                check(f"rejects invalid request id {bad_id!r}", True)

        rid = new_request_id("check")
        check("generated ids are valid", len(rid) <= 64 and
              all(ch.isalnum() or ch in "-_" for ch in rid), rid)

        # -- unreachable device ---------------------------------------------
        print("\n[unreachable device]")
        dead = PumpClient("127.0.0.1", SECRET, port=free_port(), timeout=0.4,
                          retries=2, backoff=0.05)
        try:
            dead.status()
            check("unreachable device raises PumpUnreachable", False)
        except PumpUnreachable:
            check("unreachable device raises PumpUnreachable", True)

        # -- background poller ----------------------------------------------
        print("\n[status poller]")
        poller = StatusPoller(pump, idle_interval=0.15, active_interval=0.05)
        poller.start()
        time.sleep(0.6)
        latest, updated_at, err = poller.latest()
        check("poller produced a status", latest is not None, str(err))
        check("poller recorded a timestamp", updated_at is not None)
        check("poller reports a small age", (poller.age_seconds() or 99) < 2.0,
              f"{poller.age_seconds():.2f}s")
        poller.stop_polling()
        check("poller stops cleanly", not poller.is_alive())

        # -- timing overrides -------------------------------------------------
        print("\n[timing overrides]")
        st = pump.status()
        check("status reports the timings in use",
              st.timings.crank_ms == 2000 and st.timings.choke_prep_ms == 1000,
              str(st.timings))
        check("status reports the hard crank ceiling",
              st.timings.max_crank_ms == 5000)
        check("total_start_ms includes the prime dwell",
              st.timings.total_start_ms() == 8500,
              str(st.timings.total_start_ms()))
        check("total_stop_ms includes the intake close delay",
              st.timings.total_stop_ms() == 6000,
              str(st.timings.total_stop_ms()))
        check("valve timings reported",
              st.timings.valve_prime_ms == 5000
              and st.timings.valve_close_delay_ms == 3000, str(st.timings))

        settle_to_idle(pump)
        pump.start(crank_ms=1000, choke_ms=300, unchoke_ms=200)
        st = pump.status()
        check("override is reflected in status",
              st.timings.crank_ms == 1000 and st.timings.choke_prep_ms == 300,
              str(st.timings))
        check("the hard ceiling does not move with an override",
              st.timings.max_crank_ms == 5000)

        t0 = time.monotonic()
        while (pump.status().state != "RUNNING_ASSUMED"
               and time.monotonic() - t0 < FULL_START_S + 4):
            time.sleep(0.02)
        elapsed_ms = (time.monotonic() - t0) * 1000
        # 300 + 1000 + 200 plus the fixed 5000 ms prime dwell.
        check("the shortened sequence really was shorter", elapsed_ms < 8000,
              f"{elapsed_ms:.0f} ms")

        settle_to_idle(pump)
        try:
            pump.start(crank_ms=99999)
            check("an over-ceiling crank override is rejected", False)
        except PumpClientBug as exc:
            check("an over-ceiling crank override is rejected", True, str(exc)[:60])
        check("the rejected override did not start the pump",
              pump.status().state == "IDLE")

        try:
            pump.start(crank_ms=-5)
            check("a negative override is rejected client-side", False)
        except ValueError:
            check("a negative override is rejected client-side", True)

        # Timing headers are only meaningful on START.
        try:
            pump._command("/v1/stop", "stop", None, {"X-Crank-Ms": "1000"})
            check("timing headers on /v1/stop are rejected", False)
        except PumpClientBug:
            check("timing headers on /v1/stop are rejected", True)

        # -- maintenance API is absent by default -----------------------------
        print("\n[maintenance API disabled by default]")
        check("status advertises maintenance_api=false",
              pump.status().maintenance_api is False)
        try:
            pump.maintenance("choke", True)
            check("maintenance endpoints 404 when disabled", False)
        except PumpClientBug:
            check("maintenance endpoints 404 when disabled", True)
        try:
            pump.maintenance("nonsense", True)
            check("an unknown relay name is rejected client-side", False)
        except ValueError:
            check("an unknown relay name is rejected client-side", True)

        # -- event log --------------------------------------------------------
        print("\n[event log]")
        batch = pump.drain_log(0)
        check("the log has entries", len(batch.entries) > 0,
              f"{len(batch.entries)} entries")
        check("nothing was dropped", batch.dropped == 0, str(batch.dropped))

        events = [e.event for e in batch.entries]
        states = [e.state for e in batch.entries]
        check("boot was recorded", "BOOT" in events)
        check("commands were recorded", "CMD" in events)
        check("state changes were recorded", "STATE" in events)
        for expected in ("PRIMING", "CRANKING", "RUNNING_ASSUMED", "STOPPING"):
            check(f"the log shows {expected}", expected in states,
                  str(sorted(set(states))))

        # THE property: reads are non-destructive, so a Pi that crashes after
        # receiving a batch but before writing it can just ask again.
        again = pump.drain_log(0)
        check("re-reading the same cursor returns the same entries",
              [e.seq for e in batch.entries] == [e.seq for e in again.entries])
        check("re-reading did not consume anything",
              len(again.entries) == len(batch.entries))

        # A cursor at the end yields nothing new.
        tail = pump.fetch_log(batch.next)
        check("a cursor at the end returns no entries", len(tail.entries) == 0)
        check("next does not move when there is nothing new",
              tail.next == batch.next, f"{tail.next} vs {batch.next}")

        # Entries decode into something a human can read.
        sample = batch.entries[0]
        check("entries carry a relay snapshot",
              set(sample.relays) == {"starter", "choke", "kill", "valve"},
              str(sample.relays))
        check("entries describe themselves", len(sample.describe()) > 0,
              sample.describe())

        # A malformed cursor is a client bug, not a silent zero.
        try:
            pump._request("GET", "/v1/log?since=nope")
            check("a malformed since is rejected", False)
        except PumpClientBug:
            check("a malformed since is rejected", True)
        try:
            pump.fetch_log(-1)
            check("a negative cursor is rejected client-side", False)
        except ValueError:
            check("a negative cursor is rejected client-side", True)

        # -- leave it safe ----------------------------------------------------
        settle_to_idle(pump)
        check("final state is IDLE", pump.status().state == "IDLE")

    finally:
        shutdown()

    # -- maintenance API enabled (separate instance) --------------------------
    print("\n[maintenance API enabled]")
    port2 = free_port()
    _h2, _m2, shutdown2 = serve(port2, SECRET, maintenance=True)
    time.sleep(0.2)
    maint = PumpClient("127.0.0.1", SECRET, port=port2, timeout=2.0)
    try:
        check("status advertises maintenance_api=true",
              maint.status().maintenance_api is True)
        maint.reset_idle()

        r = maint.maintenance("choke", True)
        check("manual choke on accepted", r.accepted)
        check("choke relay is energised", maint.status().relay_outputs.choke)

        # START must be refused while a relay is manually held.
        try:
            maint.start()
            check("START refused while a relay is manually energised", False)
        except PumpConflict:
            check("START refused while a relay is manually energised", True)

        maint.maintenance("choke", False)
        check("choke relay released", not maint.status().relay_outputs.choke)

        # The kill is already asserted at rest; confirm it.
        check("kill asserted at rest", maint.status().relay_outputs.kill)
        try:
            maint.maintenance("starter", True)
            check("starter refused while kill is asserted", False)
        except PumpConflict:
            check("starter refused while kill is asserted", True)
        check("starter stayed open", not maint.status().relay_outputs.starter)

        # Releasing the kill is still not enough: the pump must be primed.
        maint.maintenance("kill", False)
        try:
            maint.maintenance("starter", True)
            check("starter refused before the pump is primed", False)
        except PumpConflict:
            check("starter refused before the pump is primed", True)

        # Open the intake, but crank before the dwell elapses.
        maint.maintenance("valve", True)
        try:
            maint.maintenance("starter", True)
            check("starter refused on a freshly opened intake", False)
        except PumpConflict:
            check("starter refused on a freshly opened intake", True)

        # Now wait out the full prime dwell.
        time.sleep(VALVE_PRIME_MS / 1000 + 0.3)
        r = maint.maintenance("starter", True)
        check("starter permitted once properly primed", r.accepted)
        check("starter engaged", maint.status().relay_outputs.starter)

        # Asserting the kill releases the starter first.
        maint.maintenance("kill", True)
        s = maint.status()
        check("asserting kill released the starter first",
              s.relay_outputs.kill and not s.relay_outputs.starter,
              str(s.relay_outputs))

        # STOP still overrides everything.
        maint.maintenance("choke", True)
        maint.stop()
        s = maint.status()
        check("STOP clears manually held relays",
              not s.relay_outputs.choke and not s.relay_outputs.starter,
              str(s.relay_outputs))
        check("STOP grounded the kill circuit", s.relay_outputs.kill)
        time.sleep(FULL_STOP_S + 0.4)
    finally:
        shutdown2()

    print("\n" + "=" * 60)
    print(f"passed {_passed} | failed {_failed}")
    print("=" * 60)
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
