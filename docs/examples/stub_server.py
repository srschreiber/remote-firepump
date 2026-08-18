#!/usr/bin/env python3
"""A stand-in for the Arduino, so the Raspberry Pi side can be developed and
tested without hardware.

    python3 stub_server.py --port 8080 --secret dev-secret

It reproduces the firmware's observable behaviour: the nine states, the real
timings, the interlocks that matter to a caller, the idempotency ring, and the
exact status codes -- including the awkward ones your client must handle.

Faithfully reproduced, because these are the cases that bite:

  * UNKNOWN on start-up, and START refused from it
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

FIRMWARE_VERSION = "0.1.0"
DEVICE_NAME = "fire-pump-controller"
IDEMPOTENCY_SLOTS = 8
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")

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
        self.relays = {"starter": False, "choke": False, "kill": False,
                       "spare": False}
        self.fault = None
        self.last_starter_release_at = None
        self.last_command = None
        self._ring: list[tuple[str, str, int, bool]] = []   # id, cmd, status, accepted

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

    def _release_starter(self) -> None:
        if self.relays["starter"]:
            self.relays["starter"] = False
            self.last_starter_release_at = now_ms()

    # -- background timing -------------------------------------------------

    def tick(self) -> None:
        with self._lock:
            elapsed = now_ms() - self.state_entered_at

            if self.state == "CHOKING" and elapsed >= CHOKE_PREP_MS:
                self.relays["kill"] = False
                self.relays["starter"] = True
                self._enter("CRANKING")

            elif self.state == "CRANKING" and elapsed >= CRANK_DURATION_MS:
                self._release_starter()
                self._enter("UNCHOKING")

            elif self.state == "UNCHOKING" and elapsed >= UNCHOKE_DELAY_MS:
                self.relays["choke"] = False
                self._enter("RUNNING_ASSUMED")

            elif self.state == "STOPPING" and elapsed >= KILL_HOLD_MS:
                self.relays["kill"] = False
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
                    "CHOKING": "STOPPED_ASSUMED",
                    "CRANKING": "STARTING", "UNCHOKING": "STARTING",
                    "RUNNING_ASSUMED": "RUNNING_ASSUMED",
                    "STOPPING": "STOPPING",
                }[self.state],
                # Structurally false: there is no engine sensor.
                "running_confirmed": False,
                "relay_outputs": dict(self.relays),
                "wifi": {"connected": True, "ip": "127.0.0.1", "rssi_dbm": -42},
                "cooldown_remaining_ms": self._cooldown_remaining(),
                "last_command": self.last_command,
                "fault": self.fault,
            }

    def command(self, cmd: str, request_id: str) -> tuple[int, dict]:
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

            accepted, status = self._apply(cmd)

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

    def _apply(self, cmd: str) -> tuple[bool, int]:
        if cmd == "STOP":
            # Accepted from every state, without exception.
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = True
            self.fault = None
            self._enter("STOPPING")
            self.state_entered_at = now_ms()
            return True, 202

        if cmd == "START":
            if self.state != "IDLE" or self._cooldown_remaining() > 0:
                return False, 409
            self.relays["kill"] = False
            self.relays["starter"] = False
            self.relays["choke"] = True
            self._enter("CHOKING")
            return True, 202

        if cmd == "START_FAILED":
            if self.state not in ("RUNNING_ASSUMED", "RETRY_WAIT", "IDLE",
                                  "UNKNOWN"):
                return False, 409
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = False
            self._enter("RETRY_WAIT")
            return True, 202

        if cmd == "RESET_IDLE":
            if self.state not in ("UNKNOWN", "IDLE", "RETRY_WAIT", "FAULT"):
                return False, 409
            self._release_starter()
            self.relays["choke"] = False
            self.relays["kill"] = False
            self.fault = None
            self._enter("RETRY_WAIT" if self._cooldown_remaining() > 0 else "IDLE")
            return True, 202

        return False, 400


ROUTES = {
    ("GET", "/v1/status"): None,
    ("POST", "/v1/start"): "START",
    ("POST", "/v1/stop"): "STOP",
    ("POST", "/v1/start-failed"): "START_FAILED",
    ("POST", "/v1/reset-idle"): "RESET_IDLE",
}
KNOWN_PATHS = {p for _m, p in ROUTES}


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

        if path not in KNOWN_PATHS:
            self._error(404, "not_found", "unknown endpoint", state)
            return

        key = (method, path)
        if key not in ROUTES:
            self._error(405, "method_not_allowed",
                        "method not allowed for this path", state)
            return

        cmd = ROUTES[key]
        if cmd is None:
            self._send(200, self.model.status())
            return

        rid = self.headers.get("X-Request-ID")
        if rid is not None and not REQUEST_ID_RE.match(rid):
            self._error(400, "invalid_request_id",
                        "X-Request-ID must be 1-64 chars of A-Z a-z 0-9 - _",
                        state)
            return

        status, body = self.model.command(cmd, rid or "")
        self._send(status, body)

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PUT(self):
        self._handle("PUT")

    def do_DELETE(self):
        self._handle("DELETE")


def serve(port: int, secret: str, host: str = "127.0.0.1"):
    """Starts the stub. Returns (httpd, model, shutdown_callable)."""
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
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    _httpd, _model, shutdown = serve(args.port, args.secret, args.host)
    print(f"fire-pump stub listening on http://{args.host}:{args.port}")
    print(f"secret: {args.secret}")
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
