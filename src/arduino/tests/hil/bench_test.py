#!/usr/bin/env python3
"""Hardware-in-the-loop bench test for the fire-pump controller.

Runs the automatable parts of the bench-test checklist in README.md against a
real Arduino over the LAN, and prompts for the checks that need a human with
eyes on the relay board.

    python3 bench_test.py --host 192.168.1.50 --secret YOUR_SECRET

RUN THIS WITH THE ENGINE'S 12 V CONTACT-SIDE WIRING DISCONNECTED.
The script issues real START commands. With COM/NO/NC unwired that only
clicks relays; with the engine connected it will crank the engine.

Standard library only -- no pip install required.
"""

import argparse
import http.client
import json
import socket
import sys
import threading
import time

# --- timings, kept in sync with config.h ------------------------------------
CHOKE_PREP_MS = 1000
CRANK_DURATION_MS = 2000
MAX_CRANK_MS = 5000
UNCHOKE_DELAY_MS = 500
KILL_HOLD_MS = 3000
MIN_RECRANK_GAP_MS = 10000

# The controller serves one request per connection, so sampled timings carry
# the round-trip latency of the poll itself. Tolerances reflect that; use the
# Serial log or a scope if you need millisecond-accurate numbers.
TOLERANCE_MS = 450

GREEN, RED, YELLOW, DIM, RESET = (
    "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m")


class Results:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.failures = []

    def ok(self, name, detail=""):
        self.passed += 1
        print(f"  {GREEN}PASS{RESET} {name}" + (f" {DIM}({detail}){RESET}" if detail else ""))

    def fail(self, name, detail=""):
        self.failed += 1
        self.failures.append(f"{name}: {detail}")
        print(f"  {RED}FAIL{RESET} {name}" + (f" {DIM}({detail}){RESET}" if detail else ""))

    def skip(self, name, detail=""):
        self.skipped += 1
        print(f"  {YELLOW}SKIP{RESET} {name}" + (f" {DIM}({detail}){RESET}" if detail else ""))

    def check(self, name, condition, detail=""):
        if condition:
            self.ok(name, detail)
        else:
            self.fail(name, detail)
        return condition


class Device:
    """Minimal client for the pump API."""

    def __init__(self, host, port, secret, timeout=5.0):
        self.host = host
        self.port = port
        self.secret = secret
        self.timeout = timeout

    def request(self, method, path, secret=None, request_id=None, timeout=None):
        """Returns (status, parsed_json_or_None, raw_body)."""
        headers = {}
        use_secret = self.secret if secret is None else secret
        if use_secret is not None:
            headers["X-Pump-Secret"] = use_secret
        if request_id is not None:
            headers["X-Request-ID"] = request_id

        conn = http.client.HTTPConnection(
            self.host, self.port, timeout=timeout or self.timeout)
        try:
            conn.request(method, path, headers=headers)
            resp = conn.getresponse()
            body = resp.read().decode("utf-8", errors="replace")
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError:
                parsed = None
            return resp.status, parsed, body
        finally:
            conn.close()

    def status(self):
        code, body, _ = self.request("GET", "/v1/status")
        if code != 200 or body is None:
            raise RuntimeError(f"status query failed: HTTP {code}")
        return body

    def raw(self, payload, read_reply=True, timeout=5.0):
        """Sends arbitrary bytes on a fresh connection. Returns the reply."""
        s = socket.create_connection((self.host, self.port), timeout=timeout)
        try:
            s.sendall(payload)
            if not read_reply:
                return b""
            chunks = []
            try:
                while True:
                    d = s.recv(4096)
                    if not d:
                        break
                    chunks.append(d)
            except socket.timeout:
                pass
            return b"".join(chunks)
        finally:
            s.close()


class Sampler(threading.Thread):
    """Polls /v1/status as fast as it can and records relay transitions."""

    def __init__(self, device):
        super().__init__(daemon=True)
        self.device = device
        # NB: must not be called `_stop`. threading.Thread uses that name
        # internally and join() calls it -- shadowing it breaks join().
        self._stop_event = threading.Event()
        self.samples = []   # (monotonic_seconds, state, relays dict)
        self.error = None

    def run(self):
        while not self._stop_event.is_set():
            try:
                body = self.device.status()
                self.samples.append(
                    (time.monotonic(), body["state"], dict(body["relay_outputs"])))
            except Exception as exc:      # noqa: BLE001 - diagnostic only
                self.error = exc
            time.sleep(0.01)

    def stop(self):
        self._stop_event.set()
        self.join(timeout=3.0)

    def transitions(self, relay):
        """[(t, value)] for each change of `relay`, including the first sample."""
        out = []
        prev = None
        for t, _state, relays in self.samples:
            v = relays.get(relay)
            if v != prev:
                out.append((t, v))
                prev = v
        return out

    def active_intervals(self, relay):
        """[(start, end_or_None)] of periods where `relay` was true."""
        intervals = []
        start = None
        for t, v in self.transitions(relay):
            if v and start is None:
                start = t
            elif not v and start is not None:
                intervals.append((start, t))
                start = None
        if start is not None:
            intervals.append((start, None))
        return intervals

    def states_seen(self):
        seen = []
        for _t, state, _r in self.samples:
            if not seen or seen[-1] != state:
                seen.append(state)
        return seen


def ms(seconds):
    return int(round(seconds * 1000))


def prompt(results, name, question):
    """Manual check requiring a human observation."""
    try:
        answer = input(f"  {YELLOW}MANUAL{RESET} {question} [y/N/s=skip] ").strip().lower()
    except EOFError:
        results.skip(name, "no interactive terminal")
        return
    if answer == "y":
        results.ok(name, "confirmed by operator")
    elif answer == "s":
        results.skip(name, "skipped by operator")
    else:
        results.fail(name, "not confirmed by operator")


def wait_for_state(device, target, timeout_s):
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = device.status()["state"]
        if last == target:
            return True, last
        time.sleep(0.05)
    return False, last


def settle_to_idle(device, results=None):
    """Gets the controller to IDLE with the recrank cooldown expired."""
    st = device.status()
    if st["state"] not in ("IDLE",):
        device.request("POST", "/v1/stop", request_id=f"settle-stop-{int(time.time()*1000)}")
        time.sleep(KILL_HOLD_MS / 1000 + 0.4)
        st = device.status()
        if st["state"] != "IDLE":
            device.request("POST", "/v1/reset-idle",
                           request_id=f"settle-reset-{int(time.time()*1000)}")
            time.sleep(0.3)

    remaining = device.status().get("cooldown_remaining_ms", 0)
    if remaining:
        print(f"  {DIM}waiting {remaining} ms for the recrank cooldown...{RESET}")
        time.sleep(remaining / 1000 + 0.5)

    ok, state = wait_for_state(device, "IDLE", 15)
    if results is not None and not ok:
        results.fail("settle to IDLE", f"stuck in {state}")
    return ok


# ===========================================================================
# Tests
# ===========================================================================

def test_01_power_on_no_relay_activation(results, device, args):
    print("\n[1] Power-on causes no unintended relay activation")
    if args.assume_fresh_boot:
        st = device.status()
        relays = st["relay_outputs"]
        results.check("no relay is commanded active at boot",
                      not any(relays.values()), str(relays))
    prompt(results, "no relay clicked or lit during power-on",
           "Did the relay board stay completely quiet (no click, no LED) at power-on?")


def test_02_initial_state_unknown(results, device, args):
    print("\n[2] Initial API state is UNKNOWN")
    if not args.assume_fresh_boot:
        results.skip("initial state is UNKNOWN", "pass --assume-fresh-boot after a power cycle")
        return
    st = device.status()
    results.check("state is UNKNOWN after boot", st["state"] == "UNKNOWN", st["state"])
    results.check("engine_status is UNKNOWN", st["engine_status"] == "UNKNOWN",
                  st["engine_status"])
    results.check("running_confirmed is false", st["running_confirmed"] is False)


def test_03_start_rejected_from_unknown(results, device, args):
    print("\n[3] START is rejected from UNKNOWN")
    if device.status()["state"] != "UNKNOWN":
        results.skip("START rejected from UNKNOWN", "device is not in UNKNOWN")
        return
    code, body, _ = device.request("POST", "/v1/start", request_id="bench-start-unknown")
    results.check("START from UNKNOWN returns 409", code == 409, f"HTTP {code}")
    if body:
        results.check("409 body reports the current state", body.get("state") == "UNKNOWN",
                      str(body.get("state")))
    st = device.status()
    results.check("no relay moved", not any(st["relay_outputs"].values()))


def test_04_reset_idle(results, device, args):
    print("\n[4] reset-idle transitions to IDLE")
    code, body, _ = device.request("POST", "/v1/reset-idle", request_id="bench-reset-1")
    results.check("reset-idle returns 202", code == 202, f"HTTP {code}")
    ok, state = wait_for_state(device, "IDLE", 15)
    results.check("state becomes IDLE", ok, state)
    st = device.status()
    results.check("all relays inactive after reset-idle",
                  not any(st["relay_outputs"].values()), str(st["relay_outputs"]))


def test_05_06_07_start_sequence(results, device, args):
    print("\n[5,6,7] START sequence: choke -> starter -> release -> unchoke")
    if not settle_to_idle(device, results):
        results.skip("start sequence", "could not reach IDLE")
        return

    sampler = Sampler(device)
    sampler.start()
    time.sleep(0.2)

    code, _body, _ = device.request("POST", "/v1/start", request_id="bench-start-seq")
    if code != 202:
        sampler.stop()
        results.fail("START accepted from IDLE", f"HTTP {code}")
        return
    results.ok("START from IDLE returns 202")

    total = (CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 1.5
    time.sleep(total)
    sampler.stop()

    states = sampler.states_seen()
    print(f"  {DIM}states observed: {' -> '.join(states)}{RESET}")

    for expected in ("CHOKING", "CRANKING", "UNCHOKING", "RUNNING_ASSUMED"):
        results.check(f"state {expected} was observed", expected in states,
                      " -> ".join(states))

    choke = sampler.active_intervals("choke")
    starter = sampler.active_intervals("starter")

    results.check("choke engaged exactly once", len(choke) == 1, f"{len(choke)} periods")
    results.check("starter engaged exactly once", len(starter) == 1, f"{len(starter)} periods")

    if len(starter) == 1 and starter[0][1] is not None:
        dur = ms(starter[0][1] - starter[0][0])
        # [6] starter engagement matches the configured duration
        results.check("starter duration matches CRANK_DURATION_MS",
                      abs(dur - CRANK_DURATION_MS) <= TOLERANCE_MS,
                      f"{dur} ms, expected ~{CRANK_DURATION_MS} ms")
        # [7] starter never exceeds five seconds
        results.check("starter engagement never exceeded 5000 ms", dur <= 5000,
                      f"{dur} ms")
        results.check("starter engagement never exceeded MAX_CRANK_MS",
                      dur <= MAX_CRANK_MS, f"{dur} ms")
    else:
        results.fail("starter duration measurable", "starter never released, or never seen")

    if len(choke) == 1 and len(starter) == 1:
        # Choke leads the starter by CHOKE_PREP_MS.
        lead = ms(starter[0][0] - choke[0][0])
        results.check("choke leads the starter by CHOKE_PREP_MS",
                      abs(lead - CHOKE_PREP_MS) <= TOLERANCE_MS,
                      f"{lead} ms, expected ~{CHOKE_PREP_MS} ms")
        if choke[0][1] is not None and starter[0][1] is not None:
            tail = ms(choke[0][1] - starter[0][1])
            results.check("choke releases UNCHOKE_DELAY_MS after the starter",
                          abs(tail - UNCHOKE_DELAY_MS) <= TOLERANCE_MS,
                          f"{tail} ms, expected ~{UNCHOKE_DELAY_MS} ms")

    kill = sampler.active_intervals("kill")
    results.check("kill stayed open throughout the start", len(kill) == 0,
                  f"{len(kill)} kill periods")

    spare = sampler.active_intervals("spare")
    results.check("spare relay never activated", len(spare) == 0)

    st = device.status()
    results.check("running_confirmed is still false", st["running_confirmed"] is False)
    results.check("state is RUNNING_ASSUMED", st["state"] == "RUNNING_ASSUMED", st["state"])


def test_08_stop_during_choking(results, device, args):
    print("\n[8] STOP during CHOKING cancels choke and grounds kill")
    if not settle_to_idle(device, results):
        results.skip("STOP during CHOKING", "could not reach IDLE")
        return

    device.request("POST", "/v1/start", request_id="bench-stop-choke-start")
    time.sleep(0.25)   # comfortably inside CHOKE_PREP_MS
    pre = device.status()
    results.check("device is CHOKING before the STOP", pre["state"] == "CHOKING", pre["state"])

    code, _b, _ = device.request("POST", "/v1/stop", request_id="bench-stop-choke")
    results.check("STOP returns 202 during CHOKING", code == 202, f"HTTP {code}")

    st = device.status()
    results.check("choke released immediately", st["relay_outputs"]["choke"] is False)
    results.check("starter never engaged", st["relay_outputs"]["starter"] is False)
    results.check("kill grounded", st["relay_outputs"]["kill"] is True)
    results.check("state is STOPPING", st["state"] == "STOPPING", st["state"])

    time.sleep(KILL_HOLD_MS / 1000 + 0.6)
    st = device.status()
    results.check("kill released after the hold", st["relay_outputs"]["kill"] is False)
    results.check("state returns to IDLE", st["state"] == "IDLE", st["state"])


def test_09_stop_during_cranking(results, device, args):
    print("\n[9] STOP during CRANKING releases the starter before grounding kill")
    if not settle_to_idle(device, results):
        results.skip("STOP during CRANKING", "could not reach IDLE")
        return

    sampler = Sampler(device)
    sampler.start()
    device.request("POST", "/v1/start", request_id="bench-stop-crank-start")

    # Wait until the starter is actually engaged.
    deadline = time.monotonic() + 4
    engaged = False
    while time.monotonic() < deadline:
        if device.status()["relay_outputs"]["starter"]:
            engaged = True
            break
        time.sleep(0.02)

    if not engaged:
        sampler.stop()
        results.fail("starter engaged before the STOP", "never observed CRANKING")
        return

    code, _b, _ = device.request("POST", "/v1/stop", request_id="bench-stop-crank")
    st = device.status()
    time.sleep(0.3)
    sampler.stop()

    results.check("STOP returns 202 during CRANKING", code == 202, f"HTTP {code}")
    results.check("starter released immediately", st["relay_outputs"]["starter"] is False)
    results.check("kill grounded", st["relay_outputs"]["kill"] is True)

    # No sample may ever show starter and kill active together.
    both = [t for t, _s, r in sampler.samples if r["starter"] and r["kill"]]
    results.check("starter and kill were never active simultaneously", not both,
                  f"{len(both)} samples showed both")

    starter = sampler.active_intervals("starter")
    if starter and starter[0][1] is not None:
        dur = ms(starter[0][1] - starter[0][0])
        results.check("starter was cut short by the STOP", dur < CRANK_DURATION_MS,
                      f"{dur} ms < {CRANK_DURATION_MS} ms")

    time.sleep(KILL_HOLD_MS / 1000 + 0.6)
    ok, state = wait_for_state(device, "IDLE", 10)
    results.check("state returns to IDLE", ok, state)


def test_10_stop_from_running_assumed(results, device, args):
    print("\n[10] STOP from RUNNING_ASSUMED grounds kill for KILL_HOLD_MS")
    if not settle_to_idle(device, results):
        results.skip("STOP from RUNNING_ASSUMED", "could not reach IDLE")
        return

    device.request("POST", "/v1/start", request_id="bench-stop-run-start")
    ok, state = wait_for_state(
        device, "RUNNING_ASSUMED",
        (CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 4)
    if not ok:
        results.fail("reached RUNNING_ASSUMED", state)
        return

    sampler = Sampler(device)
    sampler.start()
    time.sleep(0.15)
    code, _b, _ = device.request("POST", "/v1/stop", request_id="bench-stop-run")
    results.check("STOP returns 202 from RUNNING_ASSUMED", code == 202, f"HTTP {code}")

    time.sleep(KILL_HOLD_MS / 1000 + 1.2)
    sampler.stop()

    kill = sampler.active_intervals("kill")
    results.check("kill was grounded exactly once", len(kill) == 1, f"{len(kill)} periods")
    if len(kill) == 1 and kill[0][1] is not None:
        dur = ms(kill[0][1] - kill[0][0])
        results.check("kill hold matches KILL_HOLD_MS",
                      abs(dur - KILL_HOLD_MS) <= TOLERANCE_MS,
                      f"{dur} ms, expected ~{KILL_HOLD_MS} ms")
    else:
        results.fail("kill hold measurable", "kill never released")

    results.check("starter never engaged during a stop",
                  len(sampler.active_intervals("starter")) == 0)


def test_11_start_rejected_outside_idle(results, device, args):
    print("\n[11] START is rejected from every state other than IDLE")

    def probe(label, expect_reject=True):
        st_before = device.status()["state"]
        code, body, _ = device.request(
            "POST", "/v1/start", request_id=f"bench-probe-{label}-{int(time.time()*1000)}")
        if expect_reject:
            results.check(f"START rejected from {st_before}", code == 409,
                          f"HTTP {code} in {st_before}")
            if body:
                results.check(f"409 from {st_before} reports state",
                              "state" in body and "cooldown_remaining_ms" in body,
                              str(body))
        return code

    if not settle_to_idle(device, results):
        results.skip("START gating probes", "could not reach IDLE")
        return

    # CHOKING / CRANKING / UNCHOKING / RUNNING_ASSUMED
    device.request("POST", "/v1/start", request_id="bench-gate-start")
    time.sleep(0.25)
    probe("choking")
    time.sleep(CHOKE_PREP_MS / 1000)
    probe("cranking")
    time.sleep(CRANK_DURATION_MS / 1000 + 0.1)
    probe("unchoking-or-running")
    time.sleep(UNCHOKE_DELAY_MS / 1000 + 0.5)
    probe("running-assumed")

    # STOPPING
    device.request("POST", "/v1/stop", request_id="bench-gate-stop")
    probe("stopping")
    time.sleep(KILL_HOLD_MS / 1000 + 0.6)

    # IDLE but inside the recrank cooldown.
    st = device.status()
    if st["state"] == "IDLE" and st["cooldown_remaining_ms"] > 0:
        code, body, _ = device.request("POST", "/v1/start", request_id="bench-gate-cooldown")
        results.check("START rejected from IDLE during the recrank cooldown",
                      code == 409, f"HTTP {code}, cooldown {st['cooldown_remaining_ms']} ms")
        if body:
            results.check("409 reports a positive cooldown",
                          body.get("cooldown_remaining_ms", 0) > 0,
                          str(body.get("cooldown_remaining_ms")))

    # RETRY_WAIT
    settle_to_idle(device)
    device.request("POST", "/v1/start", request_id="bench-gate-rw-start")
    wait_for_state(device, "RUNNING_ASSUMED",
                   (CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 4)
    device.request("POST", "/v1/start-failed", request_id="bench-gate-rw-failed")
    if device.status()["state"] == "RETRY_WAIT":
        probe("retry-wait")
    else:
        results.skip("START rejected from RETRY_WAIT", "cooldown already expired")


def test_12_duplicate_request_ids(results, device, args):
    print("\n[12] Duplicate request IDs do not repeat an operation")
    if not settle_to_idle(device, results):
        results.skip("duplicate request IDs", "could not reach IDLE")
        return

    sampler = Sampler(device)
    sampler.start()
    time.sleep(0.15)

    rid = f"bench-dup-{int(time.time())}"
    c1, b1, _ = device.request("POST", "/v1/start", request_id=rid)
    c2, b2, _ = device.request("POST", "/v1/start", request_id=rid)
    c3, b3, _ = device.request("POST", "/v1/start", request_id=rid)

    time.sleep((CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 1.5)
    sampler.stop()

    results.check("first START accepted", c1 == 202, f"HTTP {c1}")
    results.check("first START not flagged duplicate",
                  bool(b1) and b1.get("duplicate") is False, str(b1))
    results.check("replayed START flagged duplicate",
                  bool(b2) and b2.get("duplicate") is True, str(b2))
    results.check("second replay flagged duplicate",
                  bool(b3) and b3.get("duplicate") is True, str(b3))

    starter = sampler.active_intervals("starter")
    results.check("the engine cranked exactly once", len(starter) == 1,
                  f"{len(starter)} starter periods")

    # A replayed STOP must still act -- it is deliberately exempt.
    settle_to_idle(device)
    srid = f"bench-dupstop-{int(time.time())}"
    device.request("POST", "/v1/stop", request_id=srid)
    time.sleep(KILL_HOLD_MS / 1000 + 0.6)
    c, b, _ = device.request("POST", "/v1/stop", request_id=srid)
    st = device.status()
    results.check("replayed STOP is still executed", c == 202 and st["relay_outputs"]["kill"],
                  f"HTTP {c}, kill={st['relay_outputs']['kill']}")
    results.check("replayed STOP is labelled duplicate",
                  bool(b) and b.get("duplicate") is True, str(b))
    time.sleep(KILL_HOLD_MS / 1000 + 0.6)


def test_13_bad_secrets(results, device, args):
    print("\n[13] Incorrect secrets return 401 and do not change relays")
    before = device.status()["relay_outputs"]

    bad_secrets = [
        None,                             # header absent entirely
        "",
        "wrong",
        device.secret[:-1],               # one character short
        device.secret + "x",              # one character long
        device.secret.upper() if device.secret.lower() != device.secret.upper() else "CASE",
    ]
    for i, bad in enumerate(bad_secrets):
        for method, path in (("GET", "/v1/status"), ("POST", "/v1/start"),
                             ("POST", "/v1/stop"), ("POST", "/v1/reset-idle")):
            code, _b, _ = device.request(method, path, secret=bad,
                                         request_id=f"bench-bad-{i}")
            if code != 401:
                results.fail(f"401 for bad secret on {method} {path}", f"HTTP {code}")
                break
        else:
            continue
        break
    else:
        results.ok("every endpoint returns 401 for every wrong secret variant")

    after = device.status()["relay_outputs"]
    results.check("no relay changed during the 401 probes", before == after,
                  f"{before} -> {after}")

    # An unknown path must also answer 401 without the secret, so endpoints
    # are not enumerable by an unauthenticated caller.
    code, _b, _ = device.request("GET", "/v1/definitely-not-a-route", secret="wrong")
    results.check("unauthenticated unknown path returns 401, not 404", code == 401,
                  f"HTTP {code}")


def test_14_15_wifi(results, device, args):
    print("\n[14,15] Wi-Fi loss does not freeze relay timing; reconnect works")
    if not args.wifi_tests:
        results.skip("Wi-Fi disconnect during an active sequence", "pass --wifi-tests")
        results.skip("reconnect after Wi-Fi returns", "pass --wifi-tests")
        return

    if not settle_to_idle(device, results):
        results.skip("Wi-Fi tests", "could not reach IDLE")
        return

    print(f"  {YELLOW}Start a sequence, then disable the AP / unplug the router.{RESET}")
    device.request("POST", "/v1/start", request_id=f"bench-wifi-{int(time.time())}")
    prompt(results, "relay timing continued through the Wi-Fi outage",
           "Did the relays complete choke -> crank -> unchoke while Wi-Fi was down "
           "(watch the LEDs / Serial log)?")

    print(f"  {YELLOW}Now restore Wi-Fi.{RESET}")
    deadline = time.monotonic() + 120
    reconnected = False
    while time.monotonic() < deadline:
        try:
            device.status()
            reconnected = True
            break
        except Exception:                # noqa: BLE001 - expected while offline
            time.sleep(2)
    results.check("device reconnected and answered /v1/status within 120 s", reconnected)
    if reconnected:
        st = device.status()
        results.check("state was not reset by the reconnect",
                      st["state"] in ("RUNNING_ASSUMED", "IDLE", "RETRY_WAIT", "UNKNOWN"),
                      st["state"])
        results.check("no relay was re-energised by the reconnect",
                      not any(st["relay_outputs"].values()), str(st["relay_outputs"]))


def test_16_reboot_returns_to_unknown(results, device, args):
    print("\n[16] Reboot returns to UNKNOWN, not IDLE or RUNNING_ASSUMED")
    if not args.reboot_test:
        results.skip("reboot returns to UNKNOWN", "pass --reboot-test")
        return

    settle_to_idle(device)
    device.request("POST", "/v1/start", request_id=f"bench-reboot-{int(time.time())}")
    wait_for_state(device, "RUNNING_ASSUMED",
                   (CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS) / 1000 + 4)

    input(f"  {YELLOW}MANUAL{RESET} Press the Arduino RESET button now, then press Enter... ")

    deadline = time.monotonic() + 90
    st = None
    while time.monotonic() < deadline:
        try:
            st = device.status()
            break
        except Exception:                # noqa: BLE001 - expected while rebooting
            time.sleep(1)

    if st is None:
        results.fail("device came back after reset", "no response within 90 s")
        return
    results.check("state is UNKNOWN after reset", st["state"] == "UNKNOWN", st["state"])
    results.check("RUNNING_ASSUMED did not survive the reset",
                  st["state"] != "RUNNING_ASSUMED", st["state"])
    results.check("all relays inactive after reset",
                  not any(st["relay_outputs"].values()), str(st["relay_outputs"]))

    code, _b, _ = device.request("POST", "/v1/start", request_id="bench-post-reboot-start")
    results.check("START refused after reset until an operator resets to IDLE",
                  code == 409, f"HTTP {code}")


def test_17_serial_banner(results, device, args):
    print("\n[17] Serial reports hostname, IP, MAC, RSSI and port")
    prompt(results, "Serial banner shows hostname, firmware, SSID, IP, MAC, RSSI and port",
           "Does the 115200-baud Serial Monitor show Device/Firmware/Wi-Fi/IP/MAC/RSSI/HTTP?")
    prompt(results, "Serial log contains no secret",
           "Confirm the Serial log never prints the Wi-Fi password or API secret. Clean?")


def test_18_lan_reachable(results, device, args):
    print("\n[18] A computer on the same LAN can call /v1/status")
    try:
        st = device.status()
        results.ok("/v1/status reachable from this machine",
                   f"{args.host}:{args.port} state={st['state']}")
        for field in ("device", "firmware_version", "state", "state_elapsed_ms",
                      "uptime_ms", "engine_status", "running_confirmed",
                      "relay_outputs", "wifi", "cooldown_remaining_ms",
                      "last_command", "fault"):
            results.check(f"status contains '{field}'", field in st)
        for relay in ("starter", "choke", "kill", "spare"):
            results.check(f"relay_outputs contains '{relay}'", relay in st["relay_outputs"])
        for k in ("connected", "ip", "rssi_dbm"):
            results.check(f"wifi contains '{k}'", k in st["wifi"])
    except Exception as exc:             # noqa: BLE001
        results.fail("/v1/status reachable", str(exc))


def test_19_netscan(results, device, args):
    print("\n[19] Router client list / NetScan shows the device")
    prompt(results, "device visible on the LAN, preferably as fire-pump-controller",
           "Does the router client list or a NetScan show the Arduino "
           "(ideally named fire-pump-controller)?")


def test_20_malformed_http(results, device, args):
    print("\n[20] Malformed and oversized HTTP cannot leave the starter energised")
    if not settle_to_idle(device, results):
        results.skip("malformed HTTP probes", "could not reach IDLE")
        return

    secret = device.secret.encode()
    probes = [
        ("garbage bytes", b"\x00\x01\x02\xff\xfe not http at all\r\n\r\n"),
        ("no version", b"POST /v1/start\r\n\r\n"),
        ("no method", b"/v1/start HTTP/1.1\r\n\r\n"),
        ("bad header", b"POST /v1/start HTTP/1.1\r\nbroken-header-line\r\n\r\n"),
        ("oversized request line",
         b"POST /" + b"a" * 4000 + b" HTTP/1.1\r\n\r\n"),
        ("oversized header block",
         b"POST /v1/start HTTP/1.1\r\nX-Pump-Secret: " + secret + b"\r\n" +
         b"".join(b"X-Pad-%d: %s\r\n" % (i, b"f" * 120) for i in range(64)) + b"\r\n"),
        ("authenticated start with an invalid request id",
         b"POST /v1/start HTTP/1.1\r\nX-Pump-Secret: " + secret +
         b"\r\nX-Request-ID: bad id!\r\n\r\n"),
        ("null bytes mid-request",
         b"POST /v1/\x00start HTTP/1.1\r\nX-Pump-Secret: " + secret + b"\r\n\r\n"),
        ("headers never terminated",
         b"POST /v1/start HTTP/1.1\r\nX-Pump-Secret: " + secret + b"\r\n"),
    ]

    for name, payload in probes:
        try:
            reply = device.raw(payload, timeout=8.0)
            head = reply.split(b"\r\n", 1)[0].decode("ascii", "replace") if reply else "(no reply)"
        except Exception as exc:         # noqa: BLE001
            head = f"(socket error: {exc})"

        # Whatever happened, the device must still be responsive and quiet.
        try:
            st = device.status()
        except Exception as exc:         # noqa: BLE001
            results.fail(f"device survived: {name}", f"status query failed: {exc}")
            continue

        results.check(f"starter inactive after: {name}",
                      st["relay_outputs"]["starter"] is False, head)
        results.check(f"state unchanged after: {name}", st["state"] == "IDLE",
                      f"{st['state']} | {head}")

    # Slowloris: dribble a request one byte at a time, far slower than the
    # client deadline. The device must drop us and stay healthy.
    print(f"  {DIM}slow-client probe (this takes a few seconds)...{RESET}")
    try:
        s = socket.create_connection((args.host, args.port), timeout=10)
        try:
            for ch in b"GET /v1/status HTTP/1.1\r\n":
                s.sendall(bytes([ch]))
                time.sleep(0.4)
        finally:
            s.close()
    except Exception:                    # noqa: BLE001 - being dropped is the point
        pass

    time.sleep(0.5)
    try:
        st = device.status()
        results.ok("device still serves requests after a slow-client attack",
                   f"state={st['state']}")
        results.check("starter inactive after the slow-client attack",
                      st["relay_outputs"]["starter"] is False)
    except Exception as exc:             # noqa: BLE001
        results.fail("device still serves requests after a slow-client attack", str(exc))


def test_21_contact_continuity(results, device, args):
    print("\n[21] Contact-side verification (before any 12 V is applied)")
    prompt(results, "COM/NO continuity verified with a multimeter",
           "With the engine wiring still DISCONNECTED, did COM-NO read open when the "
           "relay is inactive and closed when active, on K1, K2 and K3?")
    prompt(results, "K4 confirmed permanently inactive",
           "Did K4 stay inactive throughout every test above?")


ALL_TESTS = [
    test_01_power_on_no_relay_activation,
    test_02_initial_state_unknown,
    test_03_start_rejected_from_unknown,
    test_04_reset_idle,
    test_05_06_07_start_sequence,
    test_08_stop_during_choking,
    test_09_stop_during_cranking,
    test_10_stop_from_running_assumed,
    test_11_start_rejected_outside_idle,
    test_12_duplicate_request_ids,
    test_13_bad_secrets,
    test_14_15_wifi,
    test_16_reboot_returns_to_unknown,
    test_17_serial_banner,
    test_18_lan_reachable,
    test_19_netscan,
    test_20_malformed_http,
    test_21_contact_continuity,
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="Arduino IP, e.g. 192.168.1.50")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--secret", required=True, help="value of PUMP_API_SECRET")
    ap.add_argument("--assume-fresh-boot", action="store_true",
                    help="the device was power-cycled immediately before this run")
    ap.add_argument("--wifi-tests", action="store_true",
                    help="include the interactive Wi-Fi outage tests")
    ap.add_argument("--reboot-test", action="store_true",
                    help="include the interactive reset-button test")
    ap.add_argument("--only", help="run only tests whose function name contains this")
    args = ap.parse_args()

    print("=" * 70)
    print("fire-pump-controller hardware-in-the-loop bench test")
    print(f"target: http://{args.host}:{args.port}")
    print("=" * 70)
    print(f"{RED}Verify the engine's 12 V contact-side wiring is DISCONNECTED.{RESET}")
    print("This script issues real START commands.")
    try:
        if input("Type 'yes' to continue: ").strip().lower() != "yes":
            print("aborted")
            return 2
    except EOFError:
        print("aborted: needs an interactive terminal")
        return 2

    device = Device(args.host, args.port, args.secret)
    results = Results()

    try:
        device.status()
    except Exception as exc:             # noqa: BLE001
        print(f"{RED}cannot reach the device: {exc}{RESET}")
        return 2

    for test in ALL_TESTS:
        if args.only and args.only not in test.__name__:
            continue
        try:
            test(results, device, args)
        except KeyboardInterrupt:
            print("\ninterrupted")
            break
        except Exception as exc:         # noqa: BLE001
            results.fail(test.__name__, f"unhandled exception: {exc}")

    # Always leave the device somewhere safe.
    print("\nreturning the device to a safe state...")
    try:
        device.request("POST", "/v1/stop", request_id=f"bench-final-{int(time.time())}")
        time.sleep(KILL_HOLD_MS / 1000 + 0.5)
        st = device.status()
        print(f"  final state: {st['state']}, relays {st['relay_outputs']}")
    except Exception as exc:             # noqa: BLE001
        print(f"  {RED}could not confirm the final state: {exc}{RESET}")

    print("\n" + "=" * 70)
    print(f"passed {results.passed} | failed {results.failed} | skipped {results.skipped}")
    if results.failures:
        print("\nfailures:")
        for f in results.failures:
            print(f"  - {f}")
    print("=" * 70)
    return 1 if results.failed else 0


if __name__ == "__main__":
    sys.exit(main())
