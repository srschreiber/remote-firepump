#!/usr/bin/env python3
"""A stand-in for the Arduino, so the Raspberry Pi side can be developed and
tested without hardware.

    python3 stub_server.py --port 8080 --secret dev-secret

It reproduces the firmware's observable behaviour: the nine states, the real
timings, the interlocks that matter to a caller, the idempotency ring, and the
exact status codes -- including the awkward ones your client must handle.

Faithfully reproduced, because these are the cases that bite:

  * UNKNOWN on start-up, and START refused from it
  * kill ASSERTED at rest -- K3 is wired to NC, so relay_outputs.kill is True
    whenever the engine is inhibited, including in IDLE
  * PRIMING before the crank, and VALVE_CLOSING after the kill hold
  * START refused with no water, and the starter refused before the pump is
    confirmed primed
  * START refused from IDLE while the recrank cooldown is running
  * STOP accepted from every state, always, and never 409
  * STOP exempt from duplicate suppression -- a replayed stop still stops
  * duplicate suppression for every other command, bounded to eight entries
  * 401 returned before routing, so unknown paths also answer 401
  * running_confirmed always false

Standard library only.

NOT reproduced (deliberately): relay electrical behaviour, the watchdog, Wi-Fi
loss, and the MAX_CRANK_MS backstop. This models the API, not the hardware.
Test against the real device on the bench before you ship.
"""

from __future__ import annotations

import argparse
import json
import logging
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log = logging.getLogger("stub")

# Mirrors config.h.
CHOKE_PREP_MS = 1000
CRANK_DURATION_MS = 2000
UNCHOKE_DELAY_MS = 500
KILL_HOLD_MS = 3000
MIN_RECRANK_GAP_MS = 10000
MAX_CRANK_MS = 5000
VALVE_PRIME_MS = 5000
VALVE_CLOSE_DELAY_MS = 3000

MAX_CHOKE_PREP_OVERRIDE_MS = 15000
MAX_UNCHOKE_DELAY_OVERRIDE_MS = 5000

FIRMWARE_VERSION = "0.2.0"

# Mirrors ENABLE_INTAKE_VALVE. --no-valve models a build with the electric
# intake valve deleted (manual valve permanently open + foot valve).
INTAKE_VALVE_ENABLED = True

# Modelled state of the hardwired water-available interlock. --dry starts with
# no water so the refusal path can be exercised.
WATER_OK = True
DEVICE_NAME = "fire-pump-controller"
IDEMPOTENCY_SLOTS = 8
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
UINT32_RE = re.compile(r"^[0-9]{1,10}$")

# Mirrors the firmware's ENABLE_MAINTENANCE_API. Off by default, matching the
# shipped default; --maintenance turns it on.
MAINTENANCE_ENABLED = False

# Request-line / header limits, so 413 can be exercised too.
MAX_REQUEST_LINE = 256
MAX_HEADER_BYTES = 2048


def now_ms() -> int:
    return int(time.monotonic() * 1000)


class PumpModel:
    """The firmware's state machine, as far as an HTTP caller can observe it."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.boot_at = now_ms()
        self.state = "UNKNOWN"
        self.state_entered_at = self.boot_at
        # kill=True means the kill is ASSERTED (K3 de-energised, NC contact
        # grounding the ignition). That is the resting, fail-safe position: a
        # GX390's magneto keeps running when the controller loses power, so
        # the relay must be held energised to PERMIT running.
        # valve=True means the normally-closed intake valve is OPEN.
        self.relays = {"starter": False, "choke": False, "kill": True,
                       "valve": False}
        self.valve_opened_at = None
        self.fault = None
        self.last_starter_release_at = None
        self.last_command = None
        self._ring: list[tuple[str, str, int, bool]] = []   # id, cmd, status, accepted
        self.timings = {"choke_prep_ms": CHOKE_PREP_MS,
                        "crank_ms": CRANK_DURATION_MS,
                        "unchoke_delay_ms": UNCHOKE_DELAY_MS}

    # -- helpers -----------------------------------------------------------

    def _enter(self, state: str) -> None:
        if state != self.state:
            log.info("state %s -> %s", self.state, state)
        self.state = state
        self.state_entered_at = now_ms()

    def _cooldown_remaining(self) -> int:
        if self.last_starter_release_at is None:
            return 0
        if self.relays["starter"]:
            return MIN_RECRANK_GAP_MS
        elapsed = now_ms() - self.last_starter_release_at
        return max(0, MIN_RECRANK_GAP_MS - elapsed)

    def _engine_may_be_running(self) -> bool:
        return self.state in ("CRANKING", "UNCHOKING", "RUNNING_ASSUMED",
                              "STOPPING")

    def _release_starter(self) -> None:
        if self.relays["starter"]:
            self.relays["starter"] = False
            self.last_starter_release_at = now_ms()

    # -- background timing -------------------------------------------------

    def tick(self) -> None:
        with self._lock:
            elapsed = now_ms() - self.state_entered_at

            if self.state == "PRIMING" and elapsed >= VALVE_PRIME_MS:
                self.relays["choke"] = True
                self._enter("CHOKING")

            elif self.state == "CHOKING" and elapsed >= self.timings["choke_prep_ms"]:
                # Release the kill (energise K3) only at the moment of cranking.
                self.relays["kill"] = False
                self.relays["starter"] = True
                self._enter("CRANKING")

            elif self.state == "CRANKING" and elapsed >= self.timings["crank_ms"]:
                self._release_starter()
                self._enter("UNCHOKING")

            elif self.state == "UNCHOKING" and elapsed >= self.timings["unchoke_delay_ms"]:
                self.relays["choke"] = False
                self._enter("RUNNING_ASSUMED")

            elif self.state == "STOPPING" and elapsed >= KILL_HOLD_MS:
                # The kill stays ASSERTED -- that is its resting position.
                self._enter("VALVE_CLOSING")

            elif self.state == "VALVE_CLOSING" and elapsed >= VALVE_CLOSE_DELAY_MS:
                # Only now, with the engine given time to stop, is it safe to
                # shut the intake.
                self.relays["valve"] = False
                self._enter("IDLE")

            elif self.state == "RETRY_WAIT" and self._cooldown_remaining() == 0:
                self._enter("IDLE")

    # -- API ---------------------------------------------------------------

    def status(self) -> dict:
        with self._lock:
            return {
                "device": DEVICE_NAME,
                "firmware_version": FIRMWARE_VERSION,
                "state": self.state,
                "state_elapsed_ms": now_ms() - self.state_entered_at,
                "uptime_ms": now_ms() - self.boot_at,
                "engine_status": {
                    "UNKNOWN": "UNKNOWN", "FAULT": "UNKNOWN",
                    "IDLE": "STOPPED_ASSUMED", "RETRY_WAIT": "STOPPED_ASSUMED",
                    "PRIMING": "STOPPED_ASSUMED",
                    "CHOKING": "STOPPED_ASSUMED",
                    "VALVE_CLOSING": "STOPPING",
                    "CRANKING": "STARTING", "UNCHOKING": "STARTING",
                    "RUNNING_ASSUMED": "RUNNING_ASSUMED",
                    "STOPPING": "STOPPING",
                }[self.state],
                # Structurally false: there is no engine sensor.
                "running_confirmed": False,
                "relay_outputs": dict(self.relays),
                "wifi": {"connected": True, "ip": "127.0.0.1", "rssi_dbm": -42},
                "cooldown_remaining_ms": self._cooldown_remaining(),
                "water_ok": WATER_OK,
                "intake_valve_enabled": INTAKE_VALVE_ENABLED,
                "timings": {
                    "valve_prime_ms": VALVE_PRIME_MS,
                    **self.timings,
                    "kill_hold_ms": KILL_HOLD_MS,
                    "valve_close_delay_ms": VALVE_CLOSE_DELAY_MS,
                    "min_recrank_gap_ms": MIN_RECRANK_GAP_MS,
                    # The hard ceiling never moves with an override.
                    "max_crank_ms": MAX_CRANK_MS,
                },
                "maintenance_api": MAINTENANCE_ENABLED,
                "last_command": self.last_command,
                "fault": self.fault,
            }

    def command(self, cmd: str, request_id: str,
                timings: dict | None = None) -> tuple[int, dict]:
        with self._lock:
            prior = next((r for r in self._ring
                          if r[0] == request_id and r[1] == cmd), None) \
                if request_id else None
            is_duplicate = prior is not None

            # STOP is exempt from suppression: skipping a stop is dangerous,
            # repeating one merely re-grounds the kill circuit.
            if is_duplicate and cmd != "STOP":
                return prior[2], {
                    "accepted": prior[3],
                    "state": self.state,
                    "request_id": request_id,
                    "duplicate": True,
                    "cooldown_remaining_ms": self._cooldown_remaining(),
                    "running_confirmed": False,
                }

            accepted, status = self._apply(cmd, timings)

            self.last_command = {"type": cmd, "request_id": request_id or "",
                                 "accepted": accepted}

            if accepted and request_id and not is_duplicate:
                self._ring.append((request_id, cmd, status, accepted))
                if len(self._ring) > IDEMPOTENCY_SLOTS:
                    self._ring.pop(0)

            if not accepted:
                return status, {
                    "accepted": False,
                    "error": "invalid_state_for_command",
                    "message": "command not permitted in the current state",
                    "status": status,
                    "state": self.state,
                    "cooldown_remaining_ms": self._cooldown_remaining(),
                }

            return status, {
                "accepted": True,
                "state": self.state,
                "request_id": request_id or "",
                "duplicate": is_duplicate,
                "cooldown_remaining_ms": self._cooldown_remaining(),
                "running_confirmed": False,
            }

    def _apply(self, cmd: str, timings: dict | None = None) -> tuple[bool, int]:
        if cmd.startswith("MAINT_"):
            # Only from a settled state; never mid-sequence, never from FAULT.
            if self.state not in ("IDLE", "UNKNOWN"):
                return False, 409
            _, relay, action = cmd.split("_", 2)
            relay = relay.lower()
            on = (action == "ON")

            if relay == "starter" and on:
                # Two hard interlocks: the kill must be released, and the pump
                # must be confirmed primed (intake open for the full dwell,
                # water available).
                if self.relays["kill"] or not WATER_OK:
                    return False, 409
                if INTAKE_VALVE_ENABLED:
                    if not self.relays["valve"]:
                        return False, 409
                    if self.valve_opened_at is None or \
                            now_ms() - self.valve_opened_at < VALVE_PRIME_MS:
                        return False, 409

            if relay == "valve" and not on and self._engine_may_be_running():
                return False, 409           # never shut the intake on a runner

            if relay == "kill" and on:
                self._release_starter()     # starter opens first, in order

            if relay == "starter" and not on:
                self._release_starter()
            else:
                self.relays[relay] = on
                if relay == "valve" and on:
                    self.valve_opened_at = now_ms()
            return True, 202

        if cmd == "STOP":
            # Accepted from every state, without exception. The intake is
            # deliberately left OPEN here -- shutting it on a still-spinning
            # pump would run it dry. It closes in VALVE_CLOSING.
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = True
            self.fault = None
            self._enter("STOPPING")
            self.state_entered_at = now_ms()
            return True, 202

        if cmd == "START":
            # Water must be available, the kill must be asserted (IDLE means
            # inhibited), and no relay may already be driven -- so a START can
            # never be layered on top of a manual maintenance relay.
            if (self.state != "IDLE" or self._cooldown_remaining() > 0
                    or not WATER_OK
                    or not self.relays["kill"]
                    or self.relays["starter"] or self.relays["choke"]
                    or self.relays["valve"]):
                return False, 409
            self.timings = {"choke_prep_ms": CHOKE_PREP_MS,
                            "crank_ms": CRANK_DURATION_MS,
                            "unchoke_delay_ms": UNCHOKE_DELAY_MS}
            if timings:
                self.timings.update(timings)
            self.relays["starter"] = False
            if INTAKE_VALVE_ENABLED:
                # Open the intake and prime BEFORE anything else. The kill
                # stays asserted throughout priming.
                self.relays["valve"] = True
                self.valve_opened_at = now_ms()
                self._enter("PRIMING")
            else:
                self.relays["choke"] = True
                self._enter("CHOKING")
            return True, 202

        if cmd == "START_FAILED":
            if self.state not in ("RUNNING_ASSUMED", "RETRY_WAIT", "IDLE",
                                  "UNKNOWN"):
                return False, 409
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = True     # engine did not catch: inhibit it
            self._enter("RETRY_WAIT")
            self.relays["valve"] = False   # nothing running, so shut the intake
            return True, 202

        if cmd == "RESET_IDLE":
            if self.state not in ("UNKNOWN", "IDLE", "RETRY_WAIT", "FAULT"):
                return False, 409
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = True     # resting position is inhibited
            self.fault = None
            self._enter("RETRY_WAIT" if self._cooldown_remaining() > 0 else "IDLE")
            self.relays["valve"] = False
            return True, 202

        return False, 400


ROUTES = {
    ("GET", "/v1/status"): None,
    ("POST", "/v1/start"): "START",
    ("POST", "/v1/stop"): "STOP",
    ("POST", "/v1/start-failed"): "START_FAILED",
    ("POST", "/v1/reset-idle"): "RESET_IDLE",
}
MAINT_ROUTES = {
    ("POST", f"/v1/maintenance/{relay}/{action}"):
        f"MAINT_{relay.upper()}_{action.upper()}"
    for relay in ("choke", "starter", "kill", "valve")
    for action in ("on", "off")
}

# Headers carrying per-request start-sequence overrides.
TIMING_HEADERS = {
    "X-Choke-Ms": ("choke_prep_ms", MAX_CHOKE_PREP_OVERRIDE_MS, "choke_ms_out_of_range"),
    "X-Crank-Ms": ("crank_ms", MAX_CRANK_MS, "crank_ms_out_of_range"),
    "X-Unchoke-Ms": ("unchoke_delay_ms", MAX_UNCHOKE_DELAY_OVERRIDE_MS,
                     "unchoke_ms_out_of_range"),
}


def active_routes() -> dict:
    routes = dict(ROUTES)
    if MAINTENANCE_ENABLED:
        routes.update(MAINT_ROUTES)
    return routes


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    model: PumpModel = None      # type: ignore[assignment]
    secret: str = ""

    def log_message(self, fmt, *a):
        log.debug("%s - %s", self.address_string(), fmt % a)

    # -- plumbing ----------------------------------------------------------

    def _send(self, status: int, body: dict) -> None:
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def _error(self, status: int, code: str, message: str,
               state: str | None = None) -> None:
        body = {"accepted": False, "error": code, "message": message,
                "status": status, "state": state}
        self._send(status, body)

    def _handle(self, method: str) -> None:
        # Size limits, so 413 is reachable from a test.
        if len(self.requestline) > MAX_REQUEST_LINE:
            self._error(413, "request_line_too_long", "request exceeds size limits")
            return
        header_bytes = sum(len(k) + len(v) + 4 for k, v in self.headers.items())
        if header_bytes > MAX_HEADER_BYTES:
            self._error(413, "headers_too_large", "request exceeds size limits")
            return

        # Authentication first, before routing, so endpoints are not
        # enumerable by an unauthenticated caller.
        supplied = self.headers.get("X-Pump-Secret")
        if not self.secret or supplied != self.secret:
            self._error(401, "unauthorized",
                        "missing or incorrect X-Pump-Secret", None)
            return

        path = self.path.split("?", 1)[0].split("#", 1)[0]
        state = self.model.status()["state"]

        routes = active_routes()
        known_paths = {p for _m, p in routes}

        if path not in known_paths:
            self._error(404, "not_found", "unknown endpoint", state)
            return

        key = (method, path)
        if key not in routes:
            self._error(405, "method_not_allowed",
                        "method not allowed for this path", state)
            return

        cmd = routes[key]
        if cmd is None:
            self._send(200, self.model.status())
            return

        rid = self.headers.get("X-Request-ID")
        if rid is not None and not REQUEST_ID_RE.match(rid):
            self._error(400, "invalid_request_id",
                        "X-Request-ID must be 1-64 chars of A-Z a-z 0-9 - _",
                        state)
            return

        # Optional per-request timing overrides. Out-of-range values are
        # rejected rather than clamped, so the caller knows what actually ran.
        timings = {}
        supplied = {h: self.headers.get(h) for h in TIMING_HEADERS
                    if self.headers.get(h) is not None}
        if supplied:
            if cmd != "START":
                self._error(400, "timing_override_not_applicable",
                            "timing override headers are only valid on "
                            "POST /v1/start", state)
                return
            for header, raw in supplied.items():
                key_name, ceiling, code = TIMING_HEADERS[header]
                if not UINT32_RE.match(raw.strip()):
                    self._error(400, "invalid_timing_override",
                                "timing headers must be plain decimal "
                                "milliseconds", state)
                    return
                value = int(raw)
                if value > ceiling:
                    self._error(400, code,
                                f"{header} exceeds the configured maximum",
                                state)
                    return
                timings[key_name] = value

        status, body = self.model.command(cmd, rid or "", timings or None)
        self._send(status, body)

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PUT(self):
        self._handle("PUT")

    def do_DELETE(self):
        self._handle("DELETE")


def serve(port: int, secret: str, host: str = "127.0.0.1",
          maintenance: bool = False, intake_valve: bool = True,
          water_ok: bool = True):
    """Starts the stub. Returns (httpd, model, shutdown_callable)."""
    global MAINTENANCE_ENABLED, INTAKE_VALVE_ENABLED, WATER_OK
    MAINTENANCE_ENABLED = maintenance
    INTAKE_VALVE_ENABLED = intake_valve
    WATER_OK = water_ok

    model = PumpModel()

    handler = type("BoundHandler", (Handler,), {"model": model, "secret": secret})
    httpd = ThreadingHTTPServer((host, port), handler)

    stop = threading.Event()

    def ticker():
        while not stop.is_set():
            model.tick()
            stop.wait(0.02)

    t = threading.Thread(target=ticker, daemon=True, name="stub-ticker")
    t.start()
    threading.Thread(target=httpd.serve_forever, daemon=True,
                     name="stub-http").start()

    def shutdown():
        stop.set()
        httpd.shutdown()
        httpd.server_close()

    return httpd, model, shutdown


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--secret", default="dev-secret")
    ap.add_argument("--maintenance", action="store_true",
                    help="expose /v1/maintenance/* (mirrors building the "
                         "firmware with ENABLE_MAINTENANCE_API=1)")
    ap.add_argument("--no-valve", action="store_true",
                    help="model a build with ENABLE_INTAKE_VALVE=0")
    ap.add_argument("--dry", action="store_true",
                    help="start with the water interlock reporting no water")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    _httpd, _model, shutdown = serve(args.port, args.secret, args.host,
                                     maintenance=args.maintenance,
                                     intake_valve=not args.no_valve,
                                     water_ok=not args.dry)
    print(f"fire-pump stub listening on http://{args.host}:{args.port}")
    print(f"secret: {args.secret}")
    print(f"maintenance API: {'ENABLED' if args.maintenance else 'disabled'}")
    print(f"intake valve: {'disabled' if args.no_valve else 'enabled'}")
    print(f"water interlock: {'NO WATER' if args.dry else 'water available'}")
    print("initial state: UNKNOWN (send POST /v1/reset-idle to reach IDLE)")
    print("Ctrl-C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nshutting down")
        shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
