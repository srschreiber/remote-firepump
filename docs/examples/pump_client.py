#!/usr/bin/env python3
"""Reference client for the fire-pump controller HTTP API.

Written for the Raspberry Pi that sits between the operator's browser and the
Arduino. Standard library only -- no dependencies to install.

Design notes worth preserving if you rewrite this:

  * One request ID per *logical* operation, reused across network retries.
    That is what makes a retry safe: the Arduino deduplicates on
    (request id, command), so a lost response cannot crank the engine twice.

  * Only network-level failures and 5xx are retried. A 409 is a legitimate
    answer -- the command is not permitted right now -- and retrying it just
    hammers the device. Surface it to the operator instead.

  * stop() is always safe to call and always safe to retry. STOP is exempt
    from duplicate suppression on the device precisely so that a replayed
    stop still stops the engine.

  * No safety logic lives here. No cooldown timer, no "is it safe to start"
    check, no crank duration. The Arduino owns all of that and will answer
    409 if a command is not allowed. Duplicating it creates two sources of
    truth that will drift apart.

Usage:

    from pump_client import PumpClient, PumpConflict, PumpUnreachable

    pump = PumpClient("192.168.1.50", os.environ["PUMP_API_SECRET"])

    status = pump.status()
    if status.state == "UNKNOWN":
        pump.reset_idle()

    try:
        pump.start()
    except PumpConflict as exc:
        print("not permitted:", exc.state, exc.cooldown_remaining_ms)
"""

from __future__ import annotations

import http.client
import json
import logging
import re
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Optional

log = logging.getLogger(__name__)

# Must match the firmware's accepted alphabet: A-Z a-z 0-9 - _, 1..64 chars.
_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class PumpError(Exception):
    """Base class for every failure this client raises."""


class PumpUnreachable(PumpError):
    """The device did not answer. Safe to retry with the same request ID."""


class PumpAuthError(PumpError):
    """401 -- the configured secret is wrong. Do not retry; fix the config."""


class PumpConflict(PumpError):
    """409 -- valid and authenticated, but not permitted in the current state.

    This is a normal, expected outcome (for example START during the recrank
    cooldown). Show it to the operator; do not retry automatically.
    """

    def __init__(self, message: str, state: Optional[str],
                 cooldown_remaining_ms: int, body: dict):
        super().__init__(message)
        self.state = state
        self.cooldown_remaining_ms = cooldown_remaining_ms
        self.body = body


class PumpClientBug(PumpError):
    """400/404/405/413 -- this client sent something wrong. Do not retry."""


class PumpServerError(PumpError):
    """5xx from the device. Should never happen; log and alert."""


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class RelayOutputs:
    """Commanded relay states.

    These are what the firmware is driving. They do NOT prove that a relay
    physically moved, that a contact closed, or that the engine did anything.
    """
    starter: bool = False
    choke: bool = False
    kill: bool = False
    spare: bool = False


@dataclass(frozen=True)
class WifiInfo:
    connected: bool = False
    ip: str = "0.0.0.0"
    rssi_dbm: int = 0


@dataclass(frozen=True)
class LastCommand:
    type: str = "NONE"
    request_id: str = ""
    accepted: bool = False


@dataclass(frozen=True)
class PumpStatus:
    device: str
    firmware_version: str
    state: str
    state_elapsed_ms: int
    uptime_ms: int
    engine_status: str
    running_confirmed: bool
    relay_outputs: RelayOutputs
    wifi: WifiInfo
    cooldown_remaining_ms: int
    last_command: Optional[LastCommand]
    fault: Optional[str]
    raw: dict = field(repr=False, default_factory=dict)

    # -- convenience predicates for the UI ---------------------------------

    @property
    def start_permitted(self) -> bool:
        """Whether START would be accepted right now.

        Advisory only, for greying out a button. The device is the authority
        and will answer 409 regardless of what this says.
        """
        return self.state == "IDLE" and self.cooldown_remaining_ms == 0

    @property
    def sequence_active(self) -> bool:
        """True while relay timing is in progress -- poll faster during this."""
        return self.state in ("CHOKING", "CRANKING", "UNCHOKING", "STOPPING")

    @property
    def needs_operator_reset(self) -> bool:
        """UNKNOWN after a reboot: an operator must confirm the engine state."""
        return self.state == "UNKNOWN"

    @property
    def faulted(self) -> bool:
        return self.fault is not None

    @property
    def display_text(self) -> str:
        """Operator-facing wording.

        Note that RUNNING_ASSUMED is deliberately NOT phrased as "running".
        Nothing on this system can confirm the engine is turning; the operator
        confirms it from the camera.
        """
        if self.faulted:
            return f"FAULT: {self.fault}"
        return {
            "UNKNOWN": "Status unknown - confirm the engine is stopped",
            "IDLE": ("Ready to start" if self.cooldown_remaining_ms == 0
                     else f"Cooling down ({self.cooldown_remaining_ms // 1000}s)"),
            "CHOKING": "Starting... (choke)",
            "CRANKING": "Starting... (cranking)",
            "UNCHOKING": "Starting... (releasing choke)",
            "RUNNING_ASSUMED": "Start sequence completed - check the camera",
            "STOPPING": "Stopping...",
            "RETRY_WAIT": (f"Waiting before another attempt "
                           f"({self.cooldown_remaining_ms // 1000}s)"),
        }.get(self.state, self.state)

    @classmethod
    def from_json(cls, body: dict) -> "PumpStatus":
        relays = body.get("relay_outputs", {}) or {}
        wifi = body.get("wifi", {}) or {}
        last = body.get("last_command")
        return cls(
            device=body.get("device", ""),
            firmware_version=body.get("firmware_version", ""),
            state=body.get("state", "UNKNOWN"),
            state_elapsed_ms=int(body.get("state_elapsed_ms", 0)),
            uptime_ms=int(body.get("uptime_ms", 0)),
            engine_status=body.get("engine_status", "UNKNOWN"),
            running_confirmed=bool(body.get("running_confirmed", False)),
            relay_outputs=RelayOutputs(
                starter=bool(relays.get("starter", False)),
                choke=bool(relays.get("choke", False)),
                kill=bool(relays.get("kill", False)),
                spare=bool(relays.get("spare", False)),
            ),
            wifi=WifiInfo(
                connected=bool(wifi.get("connected", False)),
                ip=wifi.get("ip", "0.0.0.0"),
                rssi_dbm=int(wifi.get("rssi_dbm", 0)),
            ),
            cooldown_remaining_ms=int(body.get("cooldown_remaining_ms", 0)),
            last_command=(LastCommand(
                type=last.get("type", "NONE"),
                request_id=last.get("request_id", ""),
                accepted=bool(last.get("accepted", False)),
            ) if isinstance(last, dict) else None),
            fault=body.get("fault"),
            raw=body,
        )


@dataclass(frozen=True)
class CommandResult:
    accepted: bool
    state: str
    request_id: str
    duplicate: bool
    cooldown_remaining_ms: int
    raw: dict = field(repr=False, default_factory=dict)


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------

def new_request_id(prefix: str) -> str:
    """A fresh ID for a new logical operation.

    Reuse the returned value across retries of the SAME operation. Generate a
    new one only when the operator asks for a genuinely new action.
    """
    rid = f"{prefix}-{uuid.uuid4().hex[:16]}"
    if not _REQUEST_ID_RE.match(rid):
        raise ValueError(f"generated an invalid request id: {rid!r}")
    return rid


class PumpClient:
    """Thread-safe client for one Arduino pump controller.

    The device serves one TCP connection at a time, so all requests are
    serialised through an internal lock. Do not defeat this by creating many
    clients pointed at the same device.
    """

    def __init__(self, host: str, secret: str, port: int = 8080,
                 timeout: float = 3.0, retries: int = 3,
                 backoff: float = 0.25):
        if not secret:
            raise ValueError("a non-empty API secret is required; the device "
                             "fails closed and answers 401 without one")
        self.host = host
        self.port = port
        self._secret = secret
        self.timeout = timeout
        self.retries = max(1, retries)
        self.backoff = backoff
        self._lock = threading.Lock()

    # -- transport ---------------------------------------------------------

    def _once(self, method: str, path: str,
              request_id: Optional[str]) -> tuple[int, Any, str]:
        headers = {"X-Pump-Secret": self._secret, "Accept": "application/json"}
        if request_id is not None:
            headers["X-Request-ID"] = request_id

        conn = http.client.HTTPConnection(self.host, self.port,
                                          timeout=self.timeout)
        try:
            conn.request(method, path, headers=headers)
            resp = conn.getresponse()
            raw = resp.read().decode("utf-8", errors="replace")
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                body = None
            return resp.status, body, raw
        finally:
            conn.close()

    def _request(self, method: str, path: str,
                 request_id: Optional[str] = None) -> dict:
        """Performs a request, retrying only what is safe to retry."""
        last_exc: Optional[Exception] = None

        with self._lock:
            for attempt in range(self.retries):
                try:
                    status, body, raw = self._once(method, path, request_id)
                except (OSError, http.client.HTTPException) as exc:
                    # No response at all. The command may or may not have been
                    # applied -- retrying with the same request ID is exactly
                    # how that ambiguity is made safe.
                    last_exc = exc
                    log.warning("pump %s %s attempt %d/%d failed: %s",
                                method, path, attempt + 1, self.retries, exc)
                    if attempt + 1 < self.retries:
                        time.sleep(self.backoff * (2 ** attempt))
                    continue

                if status in (200, 202):
                    if body is None:
                        raise PumpServerError(
                            f"non-JSON body from {path}: {raw[:200]!r}")
                    return body

                if status == 401:
                    # Never log the secret, and never retry a bad one.
                    raise PumpAuthError(
                        "401 from the pump controller: X-Pump-Secret is "
                        "missing or incorrect")

                if status == 409:
                    b = body or {}
                    raise PumpConflict(
                        b.get("message", "command not permitted in the "
                                         "current state"),
                        b.get("state"),
                        int(b.get("cooldown_remaining_ms", 0)),
                        b)

                if status in (400, 404, 405, 413):
                    b = body or {}
                    raise PumpClientBug(
                        f"HTTP {status} {b.get('error', '')}: "
                        f"{b.get('message', raw[:200])}")

                if 500 <= status < 600:
                    last_exc = PumpServerError(f"HTTP {status}: {raw[:200]}")
                    log.warning("pump %s %s returned %d (attempt %d/%d)",
                                method, path, status, attempt + 1, self.retries)
                    if attempt + 1 < self.retries:
                        time.sleep(self.backoff * (2 ** attempt))
                    continue

                raise PumpError(f"unexpected HTTP {status} from {path}: "
                                f"{raw[:200]}")

        if isinstance(last_exc, PumpServerError):
            raise last_exc
        raise PumpUnreachable(
            f"no response from {self.host}:{self.port} after "
            f"{self.retries} attempts: {last_exc}") from last_exc

    # -- API ---------------------------------------------------------------

    def status(self) -> PumpStatus:
        """GET /v1/status. Safe to call as often as your poll budget allows."""
        return PumpStatus.from_json(self._request("GET", "/v1/status"))

    def _command(self, path: str, prefix: str,
                 request_id: Optional[str]) -> CommandResult:
        # None means "generate one". An explicitly supplied value -- including
        # the empty string -- is validated, never silently replaced.
        rid = new_request_id(prefix) if request_id is None else request_id
        if not _REQUEST_ID_RE.match(rid):
            raise ValueError(
                f"invalid request id {rid!r}: must be 1-64 chars of "
                f"A-Z a-z 0-9 - _")
        body = self._request("POST", path, request_id=rid)
        return CommandResult(
            accepted=bool(body.get("accepted", False)),
            state=body.get("state", "UNKNOWN"),
            request_id=body.get("request_id", rid),
            duplicate=bool(body.get("duplicate", False)),
            cooldown_remaining_ms=int(body.get("cooldown_remaining_ms", 0)),
            raw=body,
        )

    def start(self, request_id: Optional[str] = None) -> CommandResult:
        """POST /v1/start -- begin the engine start sequence.

        Only permitted from IDLE with the recrank cooldown expired; raises
        PumpConflict otherwise. Returns as soon as the sequence has been
        accepted; it then runs on the device for roughly 3.5 s.

        Pass an explicit request_id when retrying the same operator action, so
        a lost response cannot crank the engine twice.
        """
        return self._command("/v1/start", "start", request_id)

    def stop(self, request_id: Optional[str] = None) -> CommandResult:
        """POST /v1/stop -- stop immediately, from any state.

        Never rejected. Always safe to call, always safe to retry: STOP is
        exempt from duplicate suppression on the device, so a replayed stop
        still grounds the kill circuit. Treat 202 as success and ignore the
        `duplicate` flag.
        """
        return self._command("/v1/stop", "stop", request_id)

    def start_failed(self, request_id: Optional[str] = None) -> CommandResult:
        """POST /v1/start-failed -- the operator saw that it did not start.

        Puts the device in RETRY_WAIT for the minimum recrank interval, then
        IDLE. It never retries by itself, and neither should you: a human must
        look at the camera before every attempt.
        """
        return self._command("/v1/start-failed", "failed", request_id)

    def reset_idle(self, request_id: Optional[str] = None) -> CommandResult:
        """POST /v1/reset-idle -- the operator confirms the engine is stopped.

        Used after a reboot, when the device reports UNKNOWN. Only send this
        once a human has actually verified the engine is not running; the
        device cannot check for you.
        """
        return self._command("/v1/reset-idle", "reset", request_id)


# ---------------------------------------------------------------------------
# Background poller
# ---------------------------------------------------------------------------

class StatusPoller(threading.Thread):
    """Single background poller with a cached result.

    Run exactly one of these per device and have every web request read
    `latest()`. Letting each browser client poll the Arduino directly will
    starve your own command requests -- the device handles one at a time.
    """

    def __init__(self, client: PumpClient,
                 idle_interval: float = 3.0,
                 active_interval: float = 0.35,
                 max_backoff: float = 10.0):
        super().__init__(daemon=True, name="pump-status-poller")
        self.client = client
        self.idle_interval = idle_interval
        self.active_interval = active_interval
        self.max_backoff = max_backoff

        # NB: must not be called `_stop`. threading.Thread uses that name
        # internally and join() calls it -- shadowing it breaks join().
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._status: Optional[PumpStatus] = None
        self._updated_at: Optional[float] = None
        self._error: Optional[str] = None

    def latest(self) -> tuple[Optional[PumpStatus], Optional[float], Optional[str]]:
        """Returns (status, monotonic_time_of_last_success, last_error).

        Always render the age of the last success. A stale status must never
        be presented as live.
        """
        with self._lock:
            return self._status, self._updated_at, self._error

    def age_seconds(self) -> Optional[float]:
        with self._lock:
            if self._updated_at is None:
                return None
            return time.monotonic() - self._updated_at

    def run(self) -> None:
        backoff = 1.0
        while not self._stop_event.is_set():
            try:
                status = self.client.status()
                with self._lock:
                    self._status = status
                    self._updated_at = time.monotonic()
                    self._error = None
                backoff = 1.0
                delay = (self.active_interval if status.sequence_active
                         else self.idle_interval)
            except PumpError as exc:
                with self._lock:
                    self._error = str(exc)
                log.warning("status poll failed: %s", exc)
                delay = backoff
                backoff = min(backoff * 2, self.max_backoff)

            self._stop_event.wait(delay)

    def stop_polling(self, timeout: float = 5.0) -> None:
        self._stop_event.set()
        self.join(timeout=timeout)


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import argparse
    import os
    import sys

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    ap = argparse.ArgumentParser(description="fire-pump controller CLI")
    ap.add_argument("--host", default=os.environ.get("PUMP_HOST", "192.168.1.50"))
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("command",
                    choices=["status", "start", "stop", "start-failed",
                             "reset-idle", "watch"])
    args = ap.parse_args()

    secret = os.environ.get("PUMP_API_SECRET")
    if not secret:
        print("set PUMP_API_SECRET in the environment", file=sys.stderr)
        sys.exit(2)

    pump = PumpClient(args.host, secret, port=args.port)

    try:
        if args.command == "status":
            st = pump.status()
            print(json.dumps(st.raw, indent=2))
            print(f"\n-> {st.display_text}")
            if st.state == "RUNNING_ASSUMED":
                print("   (assumed, not confirmed -- check the camera)")

        elif args.command == "watch":
            poller = StatusPoller(pump)
            poller.start()
            try:
                while True:
                    st, _at, err = poller.latest()
                    age = poller.age_seconds()
                    if st is None:
                        print(f"\rno status yet ({err})", end="", flush=True)
                    else:
                        print(f"\r{st.state:<16} {st.display_text:<52} "
                              f"age={age:4.1f}s ", end="", flush=True)
                    time.sleep(0.25)
            except KeyboardInterrupt:
                print()
            finally:
                poller.stop_polling()

        else:
            fn = {"start": pump.start, "stop": pump.stop,
                  "start-failed": pump.start_failed,
                  "reset-idle": pump.reset_idle}[args.command]
            result = fn()
            print(json.dumps(result.raw, indent=2))

    except PumpConflict as exc:
        print(f"not permitted: {exc} (state={exc.state}, "
              f"cooldown={exc.cooldown_remaining_ms} ms)", file=sys.stderr)
        sys.exit(1)
    except PumpError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
