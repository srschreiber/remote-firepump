# Raspberry Pi integration guide

**Audience:** whoever is building the Raspberry Pi web/API server that sits in
front of this Arduino.

This document is the **contract**. The Arduino firmware is finished, tested and
deployed; treat its behaviour here as fixed. If something in this document
disagrees with the firmware, the firmware wins — file it as a bug.

Read [`../README.md`](../README.md) for the hardware, wiring and safety
background. This document covers only the Pi's side of the boundary.

---

## 1. Where the Pi sits

```
iPhone / browser
  → encrypted Tailscale connection          ← the Pi owns this
  → Raspberry Pi web/API server             ← YOU ARE BUILDING THIS
  → unencrypted trusted-LAN HTTP            ← plain HTTP, port 8080
  → Arduino UNO R4 WiFi                     ← finished firmware
  → relay module → Honda GX390
```

**Division of responsibility:**

| Concern | Owner |
|---|---|
| Relay timing, choke/crank/kill sequencing | **Arduino** |
| Safety interlocks, starter ceiling, cooldown | **Arduino** |
| Deciding whether a command is legal right now | **Arduino** |
| Idempotency / duplicate suppression | **Arduino** |
| User authentication, sessions, Tailscale | **Pi** |
| Operator UX, confirmations, camera feed | **Pi** |
| History, audit log, notifications | **Pi** |
| Retry policy for *network* failures | **Pi** |

### Rules the Pi must follow

1. **Never re-implement the safety logic.** Do not add your own cooldown
   timer, your own "is it safe to start" check, or your own crank duration.
   The Arduino already enforces all of it and will reject anything illegal
   with `409`. Duplicating it means two sources of truth that will drift.
2. **Never auto-retry a failed start.** Not on a timer, not on a schedule, not
   "just once more". A human must look at the camera and decide. This is a
   hard product requirement, not a preference.
3. **`STOP` must always be reachable.** One tap, no confirmation dialog, no
   nested menu, available from every screen and every state. It is the
   emergency control.
4. **Never present `RUNNING_ASSUMED` as "running".** See §6.
5. **Never expose the Arduino to the internet.** The Pi is the only thing that
   talks to it. Port 8080 stays on the LAN.

---

## 2. Connection details

| | |
|---|---|
| Base URL | `http://<arduino-ip>:8080` |
| Transport | Plain HTTP/1.1, no TLS |
| Address | Use the DHCP-reserved IP (e.g. `192.168.1.50`), **not** `.local` |
| Connections | One request per TCP connection; the device always sends `Connection: close` |
| Concurrency | **The device serves one client at a time.** Do not open parallel requests. |

### Required headers

```http
X-Pump-Secret: <the value of PUMP_API_SECRET>
```

On every request, including `GET /v1/status`. Missing or wrong → `401`.

```http
X-Request-ID: <unique id>
```

On every state-changing request. Alphabet `A-Z a-z 0-9 - _`, length 1–64.
Invalid → `400`. Missing → accepted, but you lose duplicate protection.
**Always send one.**

### Store the secret properly

Put it in an environment variable or a `0600` file on the Pi. Never in the web
app's client-side bundle, never in a URL, never in a log line. The browser must
never see it — the Pi is the only holder.

### Do not hold connections open

There is no keep-alive, no websocket, no server-sent events. Poll
`/v1/status`. See §5 for cadence.

---

## 3. Endpoints

| Method | Path | Success | Meaning |
|---|---|---|---|
| `GET` | `/v1/status` | `200` | Full device state |
| `POST` | `/v1/start` | `202` | Begin the start sequence |
| `POST` | `/v1/stop` | `202` | Stop now, from any state |
| `POST` | `/v1/start-failed` | `202` | Operator says it did not start |
| `POST` | `/v1/reset-idle` | `202` | Operator confirms engine is stopped |

No request bodies. Any body sent is ignored.

In the default build there is **no endpoint to toggle an individual relay.**
See §3.1 for the flag-gated bench-commissioning API, which is off by default
and which you should not build an operator UI around.

### 3.1 Optional extras

**Per-request timing overrides** — `POST /v1/start` accepts three optional
headers. Omit any of them to use the device default.

| Header | Overrides | Maximum |
|---|---|---|
| `X-Choke-Ms` | choke prep | 15000 |
| `X-Crank-Ms` | starter engagement | **5000** (`max_crank_ms`, the hard ceiling) |
| `X-Unchoke-Ms` | starter-release → choke-off | 5000 |

```python
pump.start(crank_ms=3500)              # longer crank for a cold engine
```

Rules that matter to your client:

* Values are plain decimal milliseconds. Anything else → `400`
  `invalid_timing_override`.
* **Out of range is a `400`, not a silent clamp.** Read `max_crank_ms` from
  `/v1/status` and validate before sending, so you can show a useful message
  instead of surfacing a raw error.
* An override can only ever *lower* the crank time. The device's
  `MAX_CRANK_MS` backstop is unchanged, so nothing you send can extend starter
  engagement past the ceiling.
* Only valid on `/v1/start`. Anywhere else → `400`
  `timing_override_not_applicable`.
* Applies to that one run only; never persisted, never survives a reboot.

If you expose this in a UI, treat it as an advanced control — a plain `START`
with the configured defaults should be the obvious path.

**Maintenance API** — `POST /v1/maintenance/{choke,starter,kill}/{on,off}`,
compiled out unless the firmware was built with `ENABLE_MAINTENANCE_API=1`.
When disabled the paths return `404`, exactly like any unknown route; check
`maintenance_api` in `/v1/status` rather than probing.

Every interlock still applies: the starter is refused while kill is grounded
(`409`), grounding kill releases the starter first, the `MAX_CRANK_MS` and
choke backstops still fire, `STOP` still overrides everything, and manual
commands are only accepted from `IDLE`/`UNKNOWN`. `START` is refused while any
relay is manually energised.

**Do not build the operator UI around this.** It is for bench commissioning
with the 12 V side disconnected. If you surface it at all, put it behind a
"maintenance" screen that is hidden unless `maintenance_api` is `true`.

### `GET /v1/status`

```json
{
  "device": "fire-pump-controller",
  "firmware_version": "0.2.0",
  "state": "IDLE",
  "state_elapsed_ms": 12345,
  "uptime_ms": 456789,
  "engine_status": "STOPPED_ASSUMED",
  "running_confirmed": false,
  "relay_outputs": {
    "starter": false, "choke": false, "kill": true, "valve": false
  },
  "water_ok": true,
  "intake_valve_enabled": true,
  "wifi": { "connected": true, "ip": "192.168.1.50", "rssi_dbm": -57 },
  "cooldown_remaining_ms": 0,
  "timings": {
    "valve_prime_ms": 5000, "choke_prep_ms": 1000, "crank_ms": 2000,
    "unchoke_delay_ms": 500, "kill_hold_ms": 3000,
    "valve_close_delay_ms": 3000, "min_recrank_gap_ms": 10000,
    "max_crank_ms": 5000
  },
  "maintenance_api": false,
  "last_command": { "type": "STOP", "request_id": "stop-123", "accepted": true },
  "fault": null
}
```

> ### ⚠️ `"kill": true` is the NORMAL resting value
>
> This trips people up, so read it once carefully.
>
> A Honda GX390 has a **magneto ignition**: it does not stop when the
> controller loses power. K3 is therefore wired to its **normally-closed**
> contact and must be held **energised** for the engine to be *permitted* to
> run. Letting go stops the engine.
>
> `relay_outputs.kill` reports whether the kill is **ASSERTED** — the ignition
> wire grounded, the engine inhibited:
>
> | `kill` | Meaning | When |
> |---|---|---|
> | `true` | Engine inhibited (K3 de-energised) | `UNKNOWN`, `IDLE`, `PRIMING`, `CHOKING`, `STOPPING`, `VALVE_CLOSING`, `RETRY_WAIT` — i.e. **most of the time** |
> | `false` | Engine permitted to run (K3 energised) | `CRANKING`, `UNCHOKING`, `RUNNING_ASSUMED` only |
>
> **Do not render `kill: true` as an alarm or an active output.** It is the
> safe state. If you want a "engine permitted to run" indicator, that is
> `kill == false`.

| Field | Type | Notes |
|---|---|---|
| `state` | string | One of the nine states in §4 |
| `state_elapsed_ms` | uint32 | Time in the current state |
| `uptime_ms` | uint32 | **Wraps at ~49.7 days.** Do not compute durations by subtracting across a poll gap without handling wrap. |
| `engine_status` | string | `UNKNOWN` / `STOPPED_ASSUMED` / `STARTING` / `RUNNING_ASSUMED` / `STOPPING` |
| `running_confirmed` | bool | **Always `false`.** See §6. |
| `relay_outputs` | object | **Commanded** output states, not proof of anything. `kill` is inverted — see the box above. `valve` is `true` when the normally-closed **intake** valve is OPEN. |
| `water_ok` | bool | Debounced water-available interlock. `false` blocks `START` with `409`. A broken sensor wire reads as `false`. |
| `intake_valve_enabled` | bool | Whether this build has the electric intake valve. `false` means `valve` is meaningless and there is no `PRIMING` phase. |
| `cooldown_remaining_ms` | uint32 | Recrank cooldown; `START` is refused while > 0 |
| `timings` | object | Timings the current/most recent start sequence is using, **including any override**. Drive your progress UI from this, not from hardcoded constants. `max_crank_ms` is the hard ceiling and never moves. |
| `maintenance_api` | bool | Whether the device was built with the maintenance endpoints. Normally `false`. |
| `last_command` | object\|null | `null` until the first command since boot |
| `fault` | string\|null | `null` when healthy; see §7 |

### Command responses (`202`)

```json
{
  "accepted": true,
  "state": "CHOKING",
  "request_id": "start-001",
  "duplicate": false,
  "cooldown_remaining_ms": 0,
  "running_confirmed": false
}
```

`state` is the state **after** the command was applied. `202` means *accepted
and started*, not *finished* — the sequence runs asynchronously.

### Error responses

All errors are JSON:

```json
{
  "accepted": false,
  "error": "invalid_state_for_command",
  "message": "command not permitted in the current state",
  "status": 409,
  "state": "CRANKING",
  "cooldown_remaining_ms": 0
}
```

| Status | `error` values | What the Pi should do |
|---|---|---|
| `400` | `invalid_request_id`, `malformed_request_line`, `malformed_header`, `incomplete_request`, `invalid_timing_override`, `crank_ms_out_of_range`, `choke_ms_out_of_range`, `unchoke_ms_out_of_range`, `timing_override_not_applicable` | **Bug in your client.** Log loudly, do not retry. |
| `401` | `unauthorized` | Misconfigured secret. Do not retry. Alert the operator. |
| `404` | `not_found` | Wrong path. Bug in your client. |
| `405` | `method_not_allowed` | Wrong verb. Bug in your client. |
| `409` | `invalid_state_for_command` | **Expected and normal.** Show the reason from `state` / `cooldown_remaining_ms`. Do not retry blindly. |
| `413` | `headers_too_large`, `request_line_too_long`, `header_line_too_long` | You sent too much. Bug in your client. |
| `500` | serialisation failures | Should never happen. Log and alert. |

Note `401` is returned **before** routing, so an unknown path with a bad secret
also returns `401`. That is intentional.

---

## 4. The state machine

You do not need to reimplement this, but your UI must reflect it correctly.

```
UNKNOWN ─reset-idle─▶ IDLE ─start─▶ PRIMING ─▶ CHOKING ─▶ CRANKING ─▶ UNCHOKING ─▶ RUNNING_ASSUMED
                       ▲            (intake              (kill                          │
                       │             opens)              released)          start-failed│
                       │                                                                ▼
                       ├─────────────────── MIN_RECRANK_GAP ─────────────────── RETRY_WAIT
                       │
                       └─ VALVE_CLOSING ◀─ KILL_HOLD ─ STOPPING ◀─stop─ (ANY state)
                          (intake shuts)

                                        FAULT ◀── invariant violated
```


Two ordering rules the whole design turns on, both enforced on the device:

1. **The engine is never cranked before the intake prime precondition is met**
   — the valve commanded open for the full `valve_prime_ms` dwell, and
   `water_ok` where a sensor is fitted.

   Read that literally. With no water sensor (`water_sensor_fitted: false`,
   which is this install) it proves only that the valve was *told* to open
   and that time passed. It is not evidence that water reached the pump — a
   stuck valve, a blown valve fuse, a severed lead or a dry source all
   satisfy it. **Never render this as "primed" or "water confirmed".** The
   camera and the operator are the only things that know water is moving.
2. **The intake is never shut while the engine may be running** — `STOP`
   grounds the kill, holds, then waits `valve_close_delay_ms` before closing.

You do not need to implement either. Just don't build a UI that implies you
can skip them.

| State | Meaning | What the Pi should show | START allowed? |
|---|---|---|---|
| `UNKNOWN` | Just booted; engine status unproven | "Status unknown — confirm the engine is stopped" + a **Confirm stopped** button | No |
| `IDLE` | Ready; engine inhibited, intake shut | "Ready to start" (or the cooldown / no-water reason) | Yes, if `cooldown_remaining_ms == 0` **and** `water_ok` |
| `PRIMING` | Intake open, priming before the crank | "Priming… (intake open)" | No |
| `CHOKING` | Choke engaged, pre-crank | "Starting… (choke)" | No |
| `CRANKING` | Starter engaged | "Starting… (cranking)" | No |
| `UNCHOKING` | Starter released, choke releasing | "Starting… (releasing choke)" | No |
| `RUNNING_ASSUMED` | Sequence completed without error | "Start sequence completed — **check the camera**" | No |
| `STOPPING` | Kill grounded; engine winding down, intake still open | "Stopping…" | No |
| `VALVE_CLOSING` | Engine stopped; shutting the intake | "Stopped — closing intake" | No |
| `RETRY_WAIT` | Cooling down after a failed start | "Waiting before another attempt: Ns" | No |
| `FAULT` | Safety invariant tripped | "FAULT: `<fault>`" prominently + recovery action | No |

Default timings — but **read them from `/v1/status`'s `timings` object rather
than hardcoding**, since they are configurable at build time and overridable
per request:

| Field | Default | Phase |
|---|---|---|
| `valve_prime_ms` | 5000 | `PRIMING` |
| `choke_prep_ms` | 1000 | `CHOKING` |
| `crank_ms` | 2000 | `CRANKING` |
| `unchoke_delay_ms` | 500 | `UNCHOKING` |
| `kill_hold_ms` | 3000 | `STOPPING` |
| `valve_close_delay_ms` | 3000 | `VALVE_CLOSING` |
| `min_recrank_gap_ms` | 10000 | `RETRY_WAIT` |
| `max_crank_ms` | 5000 | hard ceiling, never moves |

So a default start now takes about **8.5 s** from `202` to `RUNNING_ASSUMED`
(the 5 s prime dominates), and a stop takes about **6 s** to return to `IDLE`.

Both are noticeably longer than before the intake valve existed. Size your
progress UI and any request timeouts from `timings`, not from memory. The
client exposes `timings.total_start_ms()` and `timings.total_stop_ms()`.

On a build with `intake_valve_enabled: false` there is no `PRIMING` or
`VALVE_CLOSING` phase; pass `with_valve=False` to those helpers.

---

## 5. Polling

* **Idle / steady state:** every **2–5 s** is plenty.
* **During a sequence** (`CHOKING`, `CRANKING`, `UNCHOKING`, `STOPPING`):
  every **250–500 ms**, so the UI tracks the transitions.
* **Never faster than ~10 Hz.** The device handles one client at a time; a
  tight poll loop competes with your own command requests.
* **Back off when unreachable:** 1 s → 2 s → 5 s → 10 s, capped. Do not hammer
  a device that is rebooting or off the network.

Use a short timeout (2–3 s connect and read). The device is on the LAN; if it
does not answer quickly it is not going to.

**Poll from one place.** Run a single background poller that caches the latest
status, and have all web requests read that cache. Do not let each browser
client trigger its own request to the Arduino.

---

## 6. `RUNNING_ASSUMED` is not "running"

This is the single most important thing to get right in the UI.

`running_confirmed` is **hardcoded `false`** in this firmware and always will
be until the hardware gains a real sensor. There is no RPM sensor, no
oil-pressure switch, no flow meter.

`RUNNING_ASSUMED` means only: *the firmware completed a start sequence without
tripping an interlock.* It does **not** mean the engine caught, the solenoid
engaged, a contact closed, or water is moving.

**Do not** display:
- ❌ "Pump running"
- ❌ a green "RUNNING" badge
- ❌ a ✅ tick

**Do** display something like:
- ✅ "Start sequence completed — **check the camera to confirm**"
- ✅ "Assumed running (unverified)"

And put the two follow-up actions right there:

| Camera shows | Operator taps | Pi sends |
|---|---|---|
| Engine is running | *(nothing — it worked)* | — |
| Engine did not start | **"Did not start"** | `POST /v1/start-failed` |

That is the whole design: **the operator is the sensor.**

---

## 7. Faults

When `fault` is non-null the device is in `FAULT` and will refuse `START`.

| `fault` | Cause | Severity |
|---|---|---|
| `WATER_LOST` | The water interlock dropped while the engine was running. The controller stopped the engine. | **Urgent.** The pump may have run dry. Notify immediately and do not clear without inspecting. |
| `VALVE_CLOSED_WHILE_RUNNING` | The intake was found shut with the engine turning, or a crank was attempted unprimed | **Urgent.** Same as above. |
| `STARTER_OVERRUN` | Starter was active past `MAX_CRANK_MS` — the main loop stalled during a crank | **Investigate.** Alert the operator. |
| `STARTER_KILL_CONFLICT` | Starter commanded with the kill asserted | **Investigate.** Should be impossible. |
| `CHOKE_OVERRUN` | Choke active past `MAX_CHOKE_MS` | Investigate |

Recovery: `POST /v1/stop` (lands in `IDLE`) or `POST /v1/reset-idle`. Both
clear the fault.

**Log every fault with a timestamp and notify the operator.** A fault means a
safety backstop fired; it should never be cleared silently by an automatic
process. Require a deliberate human action.

---

## 8. Request IDs and retries

The Arduino keeps the last **eight accepted** state-changing request IDs. A
repeat of the same `(request ID, command)` is not re-executed; you get the
prior result back with `"duplicate": true`.

### Generate one ID per logical operation

```python
request_id = f"start-{uuid.uuid4().hex[:16]}"   # A-Za-z0-9-_ only, ≤64 chars
```

**Reuse the same ID when retrying the same logical operation** — that is what
makes the retry safe. Generate a **new** ID only when the operator asks for a
genuinely new action.

### Retry policy by failure mode

| Failure | Retry? | How |
|---|---|---|
| Connection refused / timeout / no response | **Yes** | Same request ID, up to ~3 attempts, short backoff |
| `5xx` | Yes, once | Same request ID |
| `409` | **No** | Expected. Surface the state to the operator. |
| `400` / `401` / `404` / `405` / `413` | **No** | Your bug or your config. Fix it. |

The dangerous case this protects against: you `POST /v1/start`, the response is
lost, you retry. With the same ID the engine cranks **once**. With a fresh ID
it could crank twice.

### `STOP` is special

`STOP` is **exempt from duplicate suppression**. It is still labelled
`"duplicate": true` if the ID was seen before, but it **always executes**.

Rationale: skipping a stop is dangerous; repeating one merely re-grounds the
kill circuit for another `KILL_HOLD_MS`.

For the Pi this means:
- Retrying `STOP` is always safe.
- Ignore `"duplicate"` on a `STOP` response — treat `202` as success.
- If you are unsure whether a stop landed, **send it again.**

### Idempotency resets on reboot

The ring buffer is in RAM. After a power cycle the device is in `UNKNOWN`,
which refuses `START` until an operator resets it — so a replayed `START` from
before the reboot cannot crank anything.

---

## 9. Operator workflow

Model your UI on this, not on a generic on/off switch.

```
                    ┌─────────────────────────┐
                    │ Poll GET /v1/status     │
                    └───────────┬─────────────┘
                                ▼
   state == UNKNOWN ──▶ "Confirm the engine is stopped"
                          └─▶ POST /v1/reset-idle ──▶ IDLE

   state == IDLE, cooldown 0, water_ok ──▶ [ START ] (confirmation dialog)
                          └─▶ POST /v1/start ──▶ PRIMING…RUNNING_ASSUMED (~8.5 s)

   water_ok == false ──▶ "No water available" — START disabled, reason shown

   state == RUNNING_ASSUMED ──▶ "Check the camera"
                          ├─▶ it started  → done
                          └─▶ [ Did not start ] → POST /v1/start-failed
                                                   └─▶ RETRY_WAIT → IDLE
                                                        (operator may START again)

   ANY state ──▶ [ STOP ]  ← always visible, always enabled
                  └─▶ POST /v1/stop ──▶ STOPPING ──▶ IDLE
```

### UI requirements

* **STOP:** always visible, always enabled, no confirmation, visually distinct
  (large, red). It is the emergency control.
* **START:** confirmation dialog — this starts a real engine remotely. Disable
  it whenever `state != "IDLE"`, `cooldown_remaining_ms > 0`, or
  `water_ok == false`, and show **which** of those is blocking it.
* **Water:** surface `water_ok` prominently. It is the single condition that
  protects the pump's mechanical seal, and an operator who cannot see it will
  not understand why START is refused.
* **`kill: true` is normal.** Do not style it as an alarm. If you show a
  relay panel at all, label it "engine inhibited", not "kill active".
* **Cooldown:** show it counting down, do not just grey out the button.
* **Camera feed:** put it next to the controls. The operator must be able to
  confirm without switching apps.
* **Connectivity:** show clearly when the Arduino is unreachable. A stale
  status must never look live — display the age of the last successful poll.
* **Audit log:** record every command with operator identity, timestamp,
  request ID and the response. This controls a fire pump.

---

## 10. Reference client

A complete, dependency-free client is in
[`examples/pump_client.py`](examples/pump_client.py) — retries, backoff,
request-ID handling and typed errors. Lift it directly.

```python
from pump_client import PumpClient, PumpConflict

pump = PumpClient(host="192.168.1.50", secret=os.environ["PUMP_API_SECRET"])

status = pump.status()
print(status.state, status.cooldown_remaining_ms)

try:
    result = pump.start()          # generates a request ID, retries safely
except PumpConflict as exc:
    print("not permitted:", exc.state, exc.cooldown_remaining_ms)

pump.stop()                        # always safe to call, always safe to retry
```

There is also a machine-readable [`openapi.yaml`](openapi.yaml) if you want to
generate a client in another language.

---

## 11. Testing the Pi without the Arduino

You do not need the hardware to develop against this API.

**Option A — the real device on the bench.** With the engine's 12 V
contact-side wiring disconnected, `POST /v1/start` just clicks relays. This is
the highest-fidelity option and what you should test against before release.

**Option B — a stub server.** Implement the five endpoints with the timings in
§4 and the same status codes. Make sure your stub reproduces:

- `UNKNOWN` on start-up, and `START` → `409` from it
- `409` from `IDLE` while `cooldown_remaining_ms > 0`
- `202` with `"duplicate": true` on a replayed `START`
- `202` on a replayed `STOP` that **still executes**
- `401` for a wrong secret, including on unknown paths
- `running_confirmed` always `false`
- `400` (not a clamp) for `X-Crank-Ms` above `max_crank_ms`
- `404` on `/v1/maintenance/*` unless the maintenance build is in use

The supplied stub already does all of this; run it with `--maintenance` to
model a maintenance-enabled device.

**Option C — run the firmware's own host tests.** The state machine compiles
and runs natively; see `src/arduino/tests/`. If you want to check your
understanding of a state transition, that suite is the executable spec.

### Pi-side tests worth writing

- A lost response to `START` followed by a retry cranks the engine **once**.
- `409` never triggers an automatic retry.
- `STOP` is reachable and functional from every UI state.
- A failed start **never** auto-retries.
- `RUNNING_ASSUMED` never renders as confirmed "running".
- The API secret never appears in logs, responses or the client bundle.
- Arduino unreachable → the UI shows stale/disconnected, not a frozen status.

---

## 12. Checklist before going live

- [ ] Secret stored in an env var or `0600` file, never in the web bundle
- [ ] DHCP reservation configured for the Arduino's MAC
- [ ] Port 8080 confirmed **not** reachable from outside the LAN
- [ ] Tailscale ACLs restrict who can reach the Pi's web app
- [ ] Single background poller, cached status, sane backoff
- [ ] `STOP` reachable from every screen, no confirmation
- [ ] `START` behind a confirmation dialog
- [ ] No automatic retry anywhere in the codebase
- [ ] `RUNNING_ASSUMED` never worded as confirmed running
- [ ] `water_ok` shown to the operator, and START disabled with a reason when false
- [ ] `kill: true` not styled as an alarm — it is the safe resting state
- [ ] Progress UI driven from `timings`, not hardcoded (a start is ~8.5 s now)
- [ ] `WATER_LOST` / `VALVE_CLOSED_WHILE_RUNNING` raise an urgent notification
- [ ] Camera feed adjacent to the controls
- [ ] Audit log of every command with operator identity
- [ ] Faults raise a notification and require deliberate human clearing
- [ ] Bench-tested end to end against the real Arduino with the 12 V side
      disconnected
