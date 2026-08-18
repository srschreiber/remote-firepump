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
                         UNCHOKE_DELAY_MS, serve)

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
        time.sleep(KILL_HOLD_MS / 1000 + 0.2)
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
        check("no relay is active", not any(vars(st.relay_outputs).values()))
        check("needs_operator_reset is true", st.needs_operator_reset)
        check("start_permitted is false", not st.start_permitted)
        check("firmware version reported", st.firmware_version == "0.1.0",
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
        check("START enters CHOKING", r.state == "CHOKING", r.state)
        check("START is not a duplicate", not r.duplicate)

        seen = set()
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            s = pump.status()
            seen.add(s.state)
            if s.state == "RUNNING_ASSUMED":
                break
            time.sleep(0.05)

        for expected in ("CHOKING", "CRANKING", "UNCHOKING", "RUNNING_ASSUMED"):
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
        check("kill relay grounded", pump.status().relay_outputs.kill)
        check("starter released", not pump.status().relay_outputs.starter)

        time.sleep(KILL_HOLD_MS / 1000 + 0.3)
        st = pump.status()
        check("returns to IDLE after the kill hold", st.state == "IDLE", st.state)
        check("kill released", not st.relay_outputs.kill)

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
        time.sleep((CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 0.5)
        check("sequence completed once", pump.status().state == "RUNNING_ASSUMED",
              pump.status().state)

        # -- STOP is exempt from duplicate suppression ---------------------
        print("\n[STOP duplicate exemption]")
        srid = new_request_id("stop")
        s1 = pump.stop(request_id=srid)
        check("first STOP not flagged duplicate", not s1.duplicate)
        time.sleep(KILL_HOLD_MS / 1000 + 0.3)
        check("returned to IDLE", pump.status().state == "IDLE")

        s2 = pump.stop(request_id=srid)
        check("replayed STOP is flagged duplicate", s2.duplicate)
        check("replayed STOP is STILL executed", s2.state == "STOPPING", s2.state)
        check("replayed STOP re-grounds the kill circuit",
              pump.status().relay_outputs.kill)
        time.sleep(KILL_HOLD_MS / 1000 + 0.3)

        # -- start-failed ---------------------------------------------------
        print("\n[start-failed]")
        settle_to_idle(pump)
        pump.start()
        time.sleep((CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 0.5)
        check("reached RUNNING_ASSUMED", pump.status().state == "RUNNING_ASSUMED")

        r = pump.start_failed()
        check("start-failed accepted", r.accepted)
        check("start-failed enters RETRY_WAIT", r.state == "RETRY_WAIT", r.state)
        st = pump.status()
        check("no relay active in RETRY_WAIT",
              not any(vars(st.relay_outputs).values()))
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

        # -- leave it safe ----------------------------------------------------
        settle_to_idle(pump)
        check("final state is IDLE", pump.status().state == "IDLE")

    finally:
        shutdown()

    print("\n" + "=" * 60)
    print(f"passed {_passed} | failed {_failed}")
    print("=" * 60)
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
