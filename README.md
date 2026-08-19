# remote-firepump

Firmware for an **Arduino UNO R4 WiFi (ABX00087)** that remotely controls the
electric start, choke and ignition-kill circuits of a **Honda GX390**-powered
water pump.

The Arduino sits at the pump, joins the property's 2.4 GHz LAN, and exposes a
small authenticated HTTP/JSON API. A Raspberry Pi indoors sends high-level
commands — `START`, `STOP` — and **the Arduino owns all relay timing and every
safety interlock**. The Pi never operates individual relays.

```
iPhone
  → encrypted Tailscale connection
  → Raspberry Pi web/API server
  → unencrypted trusted local LAN HTTP
  → Arduino UNO R4 WiFi
  → 4-channel relay module
  → Honda engine controls
```

The Arduino does not run Tailscale, and **must never be port-forwarded or
exposed directly to the internet**.

---

## ⚠️ Safety first

| | |
|---|---|
| ⚡ | **Never connect 12 V to an Arduino GPIO, USB or 5 V pin.** The Arduino only ever touches the relay module's `IN1`–`IN4`, `VCC` and `GND`. |
| ⚡ | The relay `COM`/`NO`/`NC` terminals are **dry contacts**, electrically separate from the Arduino. |
| 🔒 | **Never expose port 8080 to the internet.** There is no TLS. Security is the trusted LAN plus a shared secret. |
| 🔥 | Bench-test with the engine's 12 V contact-side wiring **disconnected**. `POST /v1/start` cranks a real engine. |
| ❓ | `RUNNING_ASSUMED` is an assumption, never a measurement. This build has no engine sensor. |

---

## Building the Raspberry Pi side?

Start here instead:

| Document | Purpose |
|---|---|
| **[pump-gateway/README.md](pump-gateway/README.md)** | **The Pi side, already built.** A Go web gateway that implements this contract: Tailscale-authenticated PWA, single cached poller, command log, one high-level command per tap, systemd unit and installer. Start there unless you are writing a different client. |
| **[docs/PI_INTEGRATION.md](docs/PI_INTEGRATION.md)** | **The integration contract.** Division of responsibility, endpoint semantics, retry policy, polling cadence, operator workflow, UI requirements, go-live checklist. |
| [docs/openapi.yaml](docs/openapi.yaml) | Machine-readable OpenAPI 3.1 spec for generating clients |
| [docs/examples/pump_client.py](docs/examples/pump_client.py) | Reference Python client — retries, backoff, request IDs, typed errors, background poller |
| [docs/examples/stub_server.py](docs/examples/stub_server.py) | Faithful API stub, so the Pi can be developed without hardware |
| [docs/examples/test_examples.py](docs/examples/test_examples.py) | Runs the client against the stub (63 checks) |

The three rules that matter most: **never re-implement the safety logic**,
**never auto-retry a failed start**, and **never present `RUNNING_ASSUMED` as
confirmed running**.

---

## Contents

- [Hardware assumptions](#hardware-assumptions)
- [Wiring](#wiring)
- [Power architecture](#power-architecture)
- [Building and uploading](#building-and-uploading)
- [Network setup](#network-setup)
- [HTTP API](#http-api)
- [State machine](#state-machine)
- [Safety interlocks](#safety-interlocks)
- [Watchdog](#watchdog)
- [Testing](#testing)
- [Bench-test procedure](#bench-test-procedure)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Assumptions requiring physical verification](#assumptions-requiring-physical-verification)

---

## Hardware assumptions

* Arduino UNO R4 WiFi, model **ABX00087** (Renesas RA4M1 + on-board ESP32-S3
  for Wi-Fi). Arduino Renesas UNO board package, `WiFiS3` library.
* FQBN: `arduino:renesas_uno:unor4wifi`
* Four-channel **5 V optoisolated mechanical relay module**, assumed
  **active-low** (configurable — see [Relay polarity](#relay-polarity)).
* If the relay board has a `VCC`/`JD-VCC` jumper, it is assumed **installed**,
  so the coils are powered from the Arduino's 5 V rail. Follow your actual
  relay board's documentation if it says otherwise.
* The physical Honda key stays in the **ON/RUN** position. There is no
  separate RUN relay in this design.
* **No engine-running sensors.** Existing security cameras provide visual and
  audio confirmation; the operator is the sensor.
* Arduino GPIO is 5 V tolerant/driving on this board; the relay inputs are
  driven directly.

---

## Wiring

### Arduino → relay module (control side)

This is the complete set of connections between the Arduino and the relay
board. Nothing else is attached to the Arduino.

| Arduino UNO R4 WiFi | Relay module | Function                   |
| ------------------- | ------------ | -------------------------- |
| `5V`                | `VCC`        | Relay-board control power  |
| `GND`               | `GND`        | Shared control-side ground |
| Digital `D2`        | `IN1`        | K1 starter command         |
| Digital `D3`        | `IN2`        | K2 choke command           |
| Digital `D4`        | `IN3`        | K3 **run-enable** (NC contact — energise to permit running) |
| Digital `D5`        | `IN4`        | K4 intake valve command    |
| Digital `D6`        | —            | Water-available interlock **input** (switch to `GND`) |

`D3` and `D5` are PWM-capable but are used strictly as plain digital outputs.
There is no `analogWrite()` anywhere in this firmware.

`D0` and `D1` are deliberately unused — they are reserved for RX/TX and
possible future debugging.

### Relay contact side (dry contacts — no Arduino connection)

> Exact engine terminals and wire colours **must be verified against your
> particular Honda electric-start harness**. The colours below are the ones
> documented for the choke actuator only; no Honda harness wire colours are
> invented here.

#### K1 — starter

Imitates turning the Honda key to **START**.

| Terminal | Connection |
| -------- | ---------- |
| `COM`    | Factory-fused +12 V from the key/start control circuit |
| `NO`     | Factory starter-solenoid trigger terminal/wire (typically the solenoid `S` circuit) |
| `NC`     | Unused |

K1 **does not carry starter-motor current.** It energises the existing
starter-solenoid *control* circuit. The engine battery, starter solenoid and
heavy starter cable carry the actual starting current.

#### K2 — choke

The actuator is a **DC AutoGen 12 V choke actuator** with a prewired DPDT
reversing relay.

Actuator wiring:

| Actuator lead | Connection |
| ------------- | ---------- |
| Red   | Appropriately fused +12 V |
| Black | Battery negative / ground |
| Green | Choke trigger |

Relay K2:

| Terminal | Connection |
| -------- | ---------- |
| `COM`    | Appropriately fused +12 V |
| `NO`     | Actuator green trigger wire |
| `NC`     | Unused |

When K2 is active, `COM`→`NO` applies the trigger and **engages choke**. When
K2 is inactive the trigger is removed and the actuator returns to unchoked.

> **Verify this behaviour on the bench before connecting it to the engine.**
> If your actuator latches the opposite way, the start sequence will choke at
> the wrong time.

#### K3 — run-enable (fail-safe, uses the NC contact)

**Wire this to `NC`, not `NO`.** This is the single most important wiring
detail in the project; see the [power-loss note](#️-power-loss-does-not-stop-a-gx390--this-drove-the-k3-design).

| Terminal | Connection |
| -------- | ---------- |
| `COM`    | Verified Honda ignition-kill wire |
| `NC`     | Engine ground |
| `NO`     | Unused |

| Relay state | NC contact | Kill wire | Engine |
| --- | --- | --- | --- |
| **De-energised** (resting, power loss, reset) | closed | grounded | **cannot run** |
| **Energised** (firmware permits running) | open | floating | may run |

So the firmware must actively hold K3 energised for the whole time the engine
runs, and simply letting go stops it. **Never inject 12 V into the kill wire.**

#### K4 — intake valve

A 2" **normally-closed** 12 V solenoid valve on the pump **intake**.
De-energised = shut, so it shuts at power-on and on any power loss.

| Terminal | Connection |
| -------- | ---------- |
| `COM`    | Appropriately fused +12 V |
| `NO`     | Valve solenoid + |
| `NC`     | Unused |

The valve solenoid's negative returns to battery negative. A 2" valve can draw
1–2 A continuously while held open, so fuse it on its own feed and size the
wiring for continuous duty — it stays energised for the entire run.

**Optional.** Set `ENABLE_INTAKE_VALVE 0` in `config.h` to delete it entirely;
see the next section for why you might want to.

### Intake valve and water interlock

An electrically operated **intake** valve is the most dangerous part of this
design. If it closes while the engine runs, the pump loses water and runs dry.
The water is what lubricates and cools the mechanical seal, and Honda warn that
extended dry running destroys it.

**Software cannot protect against this.** A broken valve wire, a blown valve
fuse or a mechanically stuck valve all produce a shut intake that the firmware
has no way to detect or correct. By the time anything could notice, the seal is
already going.

#### Option A — delete the electric valve (simpler, more reliable)

If the tank line can safely stay pressurised, leave a **manual intake valve
permanently open** and fit a **foot/check valve** to retain prime. Then:

```cpp
#define ENABLE_INTAKE_VALVE 0
```

K4 goes unused and an entire class of failure disappears. This is the
recommended option where the plumbing allows it.

#### Option B — keep the valve, and treat the water interlock as REQUIRED

With `ENABLE_INTAKE_VALVE 1`, a water pressure/flow switch on the intake is
**mandatory, not optional**. A compile-time assertion enforces this:

```
An electrically operated intake valve without a water interlock can run the
pump dry and destroy the seal. Either fit the interlock
(REQUIRE_WATER_INTERLOCK=1) or delete the electric valve
(ENABLE_INTAKE_VALVE=0).
```

**Wire the switch so it grounds the ignition-kill wire directly** — in parallel
with K3's NC contact — so that losing water stops the engine even if this
Arduino is dead, hung or unplugged. That hardwired path is the real protection.

`D6` is a **second, independent** layer: the firmware reads the same switch so
it can refuse to start, report `water_ok` over the API, and shut down in an
orderly way. `INPUT_PULLUP` means a broken wire reads as "no water", so sensor
wiring failures fail safe. The reading is debounced (`WATER_DEBOUNCE_MS`) so a
pressure switch chattering at its setpoint cannot trip a running fire pump, and
a startup grace (`WATER_STARTUP_GRACE_MS`) allows the pump time to build
pressure after a crank.

### Run mode

With the physical key in ON/RUN, "run mode" means:

* Starter relay inactive
* Choke relay inactive unless starting
* Kill relay inactive/open

---

## Power architecture

One 12 V starting battery powers the engine starter and solenoid, the 12 V
choke actuator, and the Arduino via a DC/DC converter.

```
Battery positive
  → inline 5 A fuse (mounted near the battery)
  → waterproof 12/24 V → 5 V DC converter
  → converter USB-A female output
  → USB-A to USB-C cable
  → Arduino UNO R4 WiFi USB-C input

Battery negative → converter negative input
```

The Arduino's regulated `5V` pin then powers the relay module control side
(`5V`→`VCC`, `GND`→`GND`).

The Raspberry Pi is indoors on its own supply and does **not** power the
Arduino.

### ⚠️ Power loss does NOT stop a GX390 — this drove the K3 design

A GX390's magneto ignition is **independent of the battery and the electric
starter**. Cutting controller power does not stop a running engine.

An earlier revision of this firmware wired K3 to its `NO` contact ("energise to
stop") and documented power loss as the fail-safe. **That was wrong**, and with
a fail-closed intake valve it produced the worst possible outcome:

| On power loss (old, wrong design) | Result |
| --- | --- |
| K3 de-energises, `NO` opens | kill wire floats → **engine keeps running** |
| K4 de-energises, NC valve shuts | **intake closes** |
| | → pump runs dry → **mechanical seal destroyed** |

K3 is now wired to its **`NC` contact** and must be held **energised** for the
engine to be permitted to run. Power loss, a watchdog reset, a blown 5 V rail
or a snapped `IN3` wire all ground the kill wire and stop the engine.

After any reboot the firmware still makes **no assumption** about whether the
engine is running — it starts in `UNKNOWN`, with the kill asserted.

> Even so, software cannot cover a broken valve wire, a blown valve fuse or a
> mechanically stuck valve. **The hardwired water interlock is mandatory** when
> the electric intake valve is fitted. See
> [Intake valve and water interlock](#intake-valve-and-water-interlock).

### Relay polarity

Most cheap optoisolated relay boards are **active-low**: driving `IN` low
energises the coil. This is captured in exactly one place:

```cpp
// config.h
constexpr bool RELAY_ACTIVE_LOW = true;
```

Every relay operation in the firmware routes through `setRelayOutput()` in
`pump_controller.cpp`, which is the only function permitted to call
`digitalWrite()`. **If your module behaves the opposite way** — a relay clicks
when the firmware says it should be released — change that one constant to
`false` and re-upload. Nothing else needs to change.

The host test suite is compiled and run at *both* polarities on every run, so
this abstraction is proven, not assumed.

---

## Building and uploading

### 1. Install the board package

```bash
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno
```

In the IDE: **Boards Manager → "Arduino UNO R4 Boards" → Install.**

### 2. Create `arduino_secrets.h`

```bash
cd src/arduino/fire_pump_controller
cp arduino_secrets.example.h arduino_secrets.h
```

Then edit it:

```cpp
#define WIFI_SSID       "YourNetwork"        // 2.4 GHz only
#define WIFI_PASSWORD   "your-wifi-password"
#define PUMP_API_SECRET "a-long-random-secret"
```

Generate a good secret with:

```bash
openssl rand -hex 32
```

`arduino_secrets.h` is git-ignored and must never be committed. The firmware
never prints the Wi-Fi password or the API secret to Serial, and never echoes
a supplied secret over HTTP. If `PUMP_API_SECRET` is left empty the firmware
**fails closed** and answers `401` to every request, including `/v1/status`.

### 3. Compile

```bash
arduino-cli compile \
  --fqbn arduino:renesas_uno:unor4wifi \
  src/arduino/fire_pump_controller
```

### 4. Upload over USB-C

Connect the board over USB-C, find the port, and upload:

```bash
arduino-cli board list

arduino-cli upload \
  --fqbn arduino:renesas_uno:unor4wifi \
  --port COM5 \
  src/arduino/fire_pump_controller
```

Replace `COM5` with your port (`/dev/ttyACM0` on Linux,
`/dev/cu.usbmodem*` on macOS).

USB firmware updates are sufficient for this MVP. There is deliberately **no
OTA updating**.

### 5. Open the Serial Monitor at 115200

```bash
arduino-cli monitor --port COM5 --config baudrate=115200
```

At startup and after every successful reconnection the device prints:

```
Device: fire-pump-controller
Firmware: 0.1.0
Wi-Fi connected: PropertyWiFi
IP: 192.168.1.50
MAC: AA:BB:CC:DD:EE:FF
RSSI: -57 dBm
HTTP: http://192.168.1.50:8080
```

It also logs every state transition, relay change and command result — never
any secret.

---

## Network setup

The Arduino joins the existing LAN as a **station**. It does not create an
access point. It uses **DHCP**; no static IP is hardcoded in firmware.

### Finding the assigned IP

1. **Serial Monitor** — easiest. The banner above prints the IP directly.
2. **Router client list** — look for `fire-pump-controller`.
3. **LAN scan**:
   ```bash
   nmap -sn 192.168.1.0/24
   ```
   or the *NetScan* / *Fing* mobile apps.

### Why the hostname does not guarantee `.local`

The firmware sets the requested DHCP hostname before associating:

```cpp
constexpr char DEVICE_HOSTNAME[] = "fire-pump-controller";
WiFi.setHostname(DEVICE_HOSTNAME);
```

This is a **DHCP option 12 hint only**. It asks the router to list the device
under that name, and most routers honour it. It is *not* mDNS/Bonjour:

* Setting a DHCP hostname does **not** start an mDNS responder.
* **Do not depend on `fire-pump-controller.local` resolving.** It may work if
  your router synthesises DNS entries from DHCP leases, and it may not.
* The router may sanitise or ignore the name entirely.

**Always address the device by IP**, backed by a DHCP reservation.

### Configure a DHCP reservation

Pin the address so the Pi's configuration never breaks:

1. Note the Arduino's MAC from the Serial banner (e.g. `AA:BB:CC:DD:EE:FF`).
2. In the router: **DHCP / LAN → Address Reservation → Add**.
3. Bind that MAC to a fixed address, for example:
   ```
   192.168.1.50
   ```
4. Reboot the Arduino and confirm it comes back on that address.

### Reconnection behaviour

* Reconnection is fully automatic.
* **Relay timing continues unaffected while Wi-Fi is down.** The state machine
  runs from `millis()` and has no network dependency whatsoever.
* A reconnect is only ever *initiated* while the controller is quiescent (no
  relay energised, no timing sequence pending), so it can never stretch a
  starter, choke or kill window.
* Reconnecting never restarts or resumes an engine operation.

---

## HTTP API

Base URL: `http://<device-ip>:8080`

Every request requires:

```http
X-Pump-Secret: <configured secret>
```

Every state-changing request should also send:

```http
X-Request-ID: <unique id>
```

Request IDs accept `A–Z a–z 0–9 - _`, 1–64 characters. An invalid ID is
rejected with `400`. A *missing* ID is accepted but forfeits duplicate
suppression — always send one from the Pi.

One request per TCP connection; responses always carry `Connection: close`.
Request bodies are ignored — none of these endpoints take one.

### Endpoints

| Method | Path               | Purpose                                                       |
| ------ | ------------------ | ------------------------------------------------------------- |
| `GET`  | `/v1/status`       | State, relay commands, network info and timing                |
| `POST` | `/v1/start`        | Start the complete engine-start sequence                      |
| `POST` | `/v1/stop`         | Stop immediately, from any state                              |
| `POST` | `/v1/start-failed` | Operator/camera confirms the previous start did not work      |
| `POST` | `/v1/reset-idle`   | Operator confirms the engine is stopped after reboot/`UNKNOWN`|

In the default build there are **no endpoints that toggle individual relays** —
the Pi cannot bypass the interlocks. A bench-commissioning
[maintenance API](#maintenance-api-disabled-by-default) exists behind a
compile-time flag that is off by default.

### Authentication

* Missing or incorrect secret → `401`.
* The supplied secret is never logged and never echoed back.
* Comparison is constant-time and folds in the length difference, so response
  timing reveals nothing about partial matches.
* Authentication is checked **before routing**, so an unauthenticated caller
  cannot enumerate endpoints by distinguishing `401` from `404`/`405`.
* No TLS — this is a trusted-LAN design. **The API must not be exposed outside
  the LAN.**

### `GET /v1/status`

```bash
curl \
  -H "X-Pump-Secret: YOUR_SECRET" \
  http://192.168.1.50:8080/v1/status
```

```json
{
  "device": "fire-pump-controller",
  "firmware_version": "0.1.0",
  "state": "IDLE",
  "state_elapsed_ms": 12345,
  "uptime_ms": 456789,
  "engine_status": "STOPPED_ASSUMED",
  "running_confirmed": false,
  "relay_outputs": {
    "starter": false,
    "choke": false,
    "kill": false,
    "spare": false
  },
  "wifi": {
    "connected": true,
    "ip": "192.168.1.50",
    "rssi_dbm": -57
  },
  "cooldown_remaining_ms": 0,
  "timings": {
    "choke_prep_ms": 1000,
    "crank_ms": 2000,
    "unchoke_delay_ms": 500,
    "kill_hold_ms": 3000,
    "min_recrank_gap_ms": 10000,
    "max_crank_ms": 5000
  },
  "maintenance_api": false,
  "last_command": {
    "type": "STOP",
    "request_id": "stop-123",
    "accepted": true
  },
  "fault": null
}
```

`timings` reports what the current (or most recent) start sequence is actually
using, including any per-request override — so a client can render an accurate
progress bar instead of assuming the defaults. `max_crank_ms` is the hard
ceiling and never moves.

> `relay_outputs` are the Arduino's **commanded** output states. They do not
> prove the physical relay moved, that the contact closed, or that the engine
> started.
>
> `running_confirmed` is **always `false`** in this MVP. There is no RPM,
> oil-pressure, water-flow or engine-running sensor.

### `POST /v1/reset-idle`

Used after a controller reboot, once the operator has **independently
confirmed the engine is stopped**.

```bash
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: reset-001" \
  http://192.168.1.50:8080/v1/reset-idle
```

Deactivates all relays, clears any fault, honours any outstanding recrank
cooldown, and transitions to `IDLE` or `RETRY_WAIT`. Returns `202`.

Accepted from `UNKNOWN`, `IDLE`, `RETRY_WAIT` and `FAULT`. Rejected with `409`
during an active sequence. It never claims the engine is running or stopped
based on relay state alone — the operator asserts that.

### `POST /v1/start`

Allowed **only** from `IDLE`, and only once the recrank cooldown has expired.

```bash
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: start-001" \
  http://192.168.1.50:8080/v1/start
```

Accepted → `202`, and the sequence begins immediately; the response does not
wait for it to finish:

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

#### Per-request timing overrides

`POST /v1/start` accepts three optional headers. Omitted headers use the
`config.h` defaults.

| Header | Overrides | Maximum |
| ------ | --------- | ------- |
| `X-Choke-Ms` | `CHOKE_PREP_MS` | `MAX_CHOKE_PREP_OVERRIDE_MS` (15000) |
| `X-Crank-Ms` | `CRANK_DURATION_MS` | **`MAX_CRANK_MS` (5000)** |
| `X-Unchoke-Ms` | `UNCHOKE_DELAY_MS` | `MAX_UNCHOKE_DELAY_OVERRIDE_MS` (5000) |

```bash
# A longer crank for a cold engine
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: start-cold-001" \
  -H "X-Crank-Ms: 3500" \
  http://192.168.1.50:8080/v1/start
```

Rules:

* Values must be plain decimal milliseconds. Anything else → `400`
  `invalid_timing_override`.
* **Out-of-range values are rejected with `400`, not silently clamped**, so the
  caller always knows exactly what ran.
* `X-Crank-Ms` can only ever *lower* the crank time. The `MAX_CRANK_MS`
  backstop in `enforceSafety()` is unchanged and still force-releases the
  starter, so no request can extend starter engagement past the hard ceiling.
  The controller re-clamps on the way in as well, so even a caller bypassing
  the HTTP layer cannot exceed it.
* These headers are only valid on `/v1/start`. Sending them anywhere else →
  `400` `timing_override_not_applicable`.
* Overrides apply to that one run only and never survive a reboot.

Not permitted → `409`, with the current state and cooldown remaining:

```json
{
  "accepted": false,
  "error": "invalid_state_for_command",
  "message": "command not permitted in the current state",
  "status": 409,
  "state": "UNKNOWN",
  "cooldown_remaining_ms": 0
}
```

### `POST /v1/stop`

**STOP has priority over every other operation.**

```bash
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: stop-001" \
  http://192.168.1.50:8080/v1/stop
```

A valid authenticated STOP is accepted from **every** state — `UNKNOWN`,
`IDLE`, `CHOKING`, `CRANKING`, `UNCHOKING`, `RUNNING_ASSUMED`, `RETRY_WAIT`,
`STOPPING`, `FAULT` — and returns `202`. It is never rejected merely because
another operation is active.

It immediately deactivates the starter, deactivates the choke, activates the
kill relay, enters `STOPPING`, holds kill for `KILL_HOLD_MS`, releases it, and
enters `IDLE`.

### `POST /v1/start-failed`

Sent after the operator checks the camera and determines the engine did not
start.

```bash
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: failed-001" \
  http://192.168.1.50:8080/v1/start-failed
```

Ensures starter, choke and kill are all inactive, enters `RETRY_WAIT`,
enforces the minimum interval between starter attempts, and transitions to
`IDLE` once it expires. Returns `202`.

**It never initiates an automatic retry.** The operator must send a fresh
`START`.

Accepted from `RUNNING_ASSUMED` (the normal case), and also from `RETRY_WAIT`,
`IDLE` and `UNKNOWN`, where it is a safe no-op that re-asserts "all relays
off". Rejected with `409` during an active sequence or from `FAULT`.

### Serial bench console (disabled by default)

The HTTP API is unreachable until Wi-Fi is configured, which leaves no way to
prove the Arduino-to-relay wiring on a fresh install. The serial console gives
you one over the USB cable alone.

Enable in `config.h`, then upload:

```cpp
#define ENABLE_SERIAL_CONSOLE 1
```

Open the Serial Monitor at 115200 and type commands (newline-terminated):

| Command | Effect |
| ------- | ------ |
| `help` | list commands |
| `status` | state, relay outputs, **live pin levels**, Wi-Fi, fault |
| `test` | **lamp test** — pulses K2, K1, K3 in turn, ~600 ms each |
| `scan` | list visible 2.4 GHz networks (SSIDs are case-sensitive!) |
| `choke on\|off`, `starter on\|off`, `kill on\|off` | drive one relay |
| `start`, `stop`, `failed`, `reset` | the normal API commands |

`test` is the fastest way to prove the wiring: you should see `IN2`, then
`IN1`, then `IN3` light and click in that order, with `IN4` never touched.

`scan` is how you confirm an SSID exactly — it is case-sensitive, and the
UNO R4 WiFi has **no 5 GHz radio**, so a network that does not appear in the
scan cannot be joined.

Every command goes through `PumpController`, so every interlock still applies.
The lamp test refuses to run outside `IDLE`/`UNKNOWN`, and its starter pulse is
far below `MAX_CRANK_MS` (which still applies regardless).

> ⚠️ **Turn this off before the controller goes on the engine.** It drives real
> relays, and a stray keystroke would crank. Anyone with USB access can already
> reflash the board, so it adds no *remote* attack surface — but it is a
> commissioning tool, not a deployment feature.

### Maintenance API (disabled by default)

For bench commissioning, the firmware can expose direct relay control:

```
POST /v1/maintenance/choke/on      POST /v1/maintenance/choke/off
POST /v1/maintenance/starter/on    POST /v1/maintenance/starter/off
POST /v1/maintenance/kill/on       POST /v1/maintenance/kill/off
```

**These are compiled out by default.** Enable in `config.h`:

```cpp
#define ENABLE_MAINTENANCE_API 1
```

When disabled the routes are indistinguishable from any other unknown path —
they return `404`, not `403`. `GET /v1/status` reports `"maintenance_api"` so a
client can tell without probing.

```bash
curl -X POST \
  -H "X-Pump-Secret: YOUR_SECRET" \
  -H "X-Request-ID: maint-001" \
  http://192.168.1.50:8080/v1/maintenance/choke/on
```

**Every hard interlock still applies**, even with the flag on:

* The starter cannot be energised while the kill circuit is grounded → `409`
  (a refusal, not a fault).
* Grounding kill still releases the starter first, in that order.
* A manually engaged starter is still force-released at `MAX_CRANK_MS`, and a
  manually engaged choke at `MAX_CHOKE_MS`. Both raise the usual fault.
* `STOP` still overrides everything, including manually held relays.
* Manual commands are only accepted from `IDLE` or `UNKNOWN` — never
  mid-sequence, never from `FAULT` → `409`.
* `START` is refused while any relay is manually energised. (This tightening
  applies unconditionally: `startPermitted()` now requires all relays at rest.
  In normal operation `IDLE` always satisfies that, so nothing else changes.)

> ⚠️ **Do not ship this enabled on a controller wired to a real engine.**
> Direct relay control bypasses the sequencing that makes the normal API safe.
> It is a screwdriver, not a feature.

### Error responses

All errors return JSON.

| Status | Meaning |
| ------ | ------- |
| `400` | Malformed request, invalid `X-Request-ID`, or a bad/out-of-range timing override |
| `401` | Missing or incorrect secret |
| `404` | Unknown endpoint |
| `405` | Wrong HTTP method for a known path |
| `409` | Command invalid for the current state (includes `state` and `cooldown_remaining_ms`) |
| `413` | Request line or header block too large |
| `500` | Internal fault (response serialisation failure) |

Limits: request line ≤ 256 bytes, total headers ≤ 2 KB, any single header line
≤ 256 bytes. Clients that stall mid-request are dropped after 3 seconds.

### Idempotency

A duplicate network request must not crank the engine twice.

The controller keeps a fixed-size ring buffer of the **last eight** accepted
state-changing request IDs and their command types and results. A repeat of
the same `(request ID, command)` pair is not executed again; the prior
acceptance result is returned along with the *current* state, and the response
carries `"duplicate": true`.

Notes:

* **`STOP` is deliberately exempt from duplicate suppression.** It is still
  *labelled* `"duplicate": true`, but it always executes. Skipping a stop is
  dangerous; repeating one merely re-grounds the kill circuit. Without this
  exemption a stale `STOP` retransmission arriving after a later `START` could
  be silently swallowed while the engine was cranking. (This was found by the
  property-based fuzzer — see [Testing](#testing).) A replayed `STOP` also
  does not consume a ring slot, so it cannot flush the history.
* Only **accepted** commands are recorded. A rejected command is not a
  committed operation, so replaying its ID later is re-evaluated on merit.
* There are no unbounded dynamic collections anywhere.
* Idempotency resets on power cycle. That is safe: after a reboot the
  controller is in `UNKNOWN`, which forbids `START` until an operator resets
  it to `IDLE`.

---

## State machine

```
                    ┌─────────┐
   power-on ───────▶│ UNKNOWN │◀──────── watchdog reset
                    └────┬────┘
                         │ reset-idle
                         ▼
   ┌──────────────▶┌─────────┐
   │               │  IDLE   │
   │               └────┬────┘
   │                    │ start  (only from IDLE, cooldown expired)
   │                    ▼
   │               ┌─────────┐
   │               │ CHOKING │  choke on
   │               └────┬────┘
   │                    │ CHOKE_PREP_MS
   │                    ▼
   │               ┌──────────┐
   │               │ CRANKING │  starter on
   │               └────┬─────┘
   │                    │ CRANK_DURATION_MS   (hard ceiling MAX_CRANK_MS)
   │                    ▼
   │              ┌───────────┐
   │              │ UNCHOKING │  starter off
   │              └────┬──────┘
   │                   │ UNCHOKE_DELAY_MS
   │                   ▼
   │          ┌─────────────────┐
   │          │ RUNNING_ASSUMED │  choke off
   │          └────┬───────┬────┘
   │               │       │ start-failed
   │      stop     │       ▼
   │               │  ┌────────────┐
   │               │  │ RETRY_WAIT │
   │               │  └─────┬──────┘
   │               │        │ MIN_RECRANK_GAP_MS
   │               ▼        │
   │          ┌──────────┐  │
   │          │ STOPPING │  │  kill on
   │          └────┬─────┘  │
   │               │ KILL_HOLD_MS
   └───────────────┴────────┘

  stop ────────────▶ STOPPING     (from ANY state, always)
  invariant violated ──▶ FAULT    (reported via /v1/status)
```

Initial state after **every** power-on or watchdog reset is `UNKNOWN`, with
all relays inactive. `RUNNING_ASSUMED` never persists across a reboot.

### Configurable timings

```cpp
// config.h
constexpr uint32_t CHOKE_PREP_MS       = 1000;   // choke before cranking
constexpr uint32_t CRANK_DURATION_MS   = 2000;   // nominal starter engagement
constexpr uint32_t MAX_CRANK_MS        = 5000;   // absolute starter ceiling
constexpr uint32_t UNCHOKE_DELAY_MS    = 500;    // starter release → choke off
constexpr uint32_t KILL_HOLD_MS        = 3000;   // kill circuit hold
constexpr uint32_t MIN_RECRANK_GAP_MS  = 10000;  // minimum gap between cranks
constexpr uint32_t MAX_CHOKE_MS        = 30000;  // choke backstop
```

Validated at compile time:

```cpp
static_assert(CRANK_DURATION_MS <= MAX_CRANK_MS, ...);
static_assert(MAX_CRANK_MS <= 5000, ...);
static_assert(CHOKE_PREP_MS + MAX_CRANK_MS + UNCHOKE_DELAY_MS < MAX_CHOKE_MS, ...);
```

Default starter engagement is **two seconds**, and it can **never** remain
active for more than **five seconds** on any normal or fault path.

### Start sequence

1. Ensure the kill relay is inactive/open.
2. Ensure the starter is inactive.
3. Activate choke.
4. Enter `CHOKING`.
5. After `CHOKE_PREP_MS`, activate starter → `CRANKING`.
6. After `CRANK_DURATION_MS`, deactivate starter → `UNCHOKING`.
7. Record the exact time the starter was released (this anchors the recrank
   cooldown).
8. After `UNCHOKE_DELAY_MS`, deactivate choke.
9. Enter `RUNNING_ASSUMED`.

There is **no automatic retry** if the engine fails to start.

### Stop sequence

1. Immediately deactivate the starter.
2. Immediately deactivate the choke.
3. Activate kill.
4. Enter `STOPPING`.
5. Hold kill active for `KILL_HOLD_MS`.
6. Deactivate kill.
7. Enter `IDLE`.

### `RUNNING_ASSUMED` vs confirmed running

**These are not the same thing, and the difference matters.**

`RUNNING_ASSUMED` means only: *the firmware completed a start sequence without
error.* The starter was commanded for the configured time, the choke was
released, and no interlock tripped.

It does **not** mean:

* the engine actually caught,
* the starter solenoid actually engaged,
* a relay contact actually closed,
* the pump is moving water.

`running_confirmed` is therefore hardcoded `false` and always will be until
this build gains a real sensor. **Confirmation comes from the operator** via
the existing security cameras — sight and sound. If the camera shows the
engine did not start, send `POST /v1/start-failed`.

---

## Safety interlocks

Enforced at the **lowest relay-control layer**, not just in API handlers — so
they hold even if a future change introduces a state-machine bug.

| Interlock | Enforcement |
| --------- | ----------- |
| Starter and kill never active simultaneously | `setStarterRelay()` refuses and faults; `enforceSafety()` re-checks every tick |
| Kill activation always releases the starter first | `setKillRelay()` drops the starter before closing kill, in that order |
| STOP overrides every state and command | `handleCommand()` handles `STOP` before any state gating; never rejected |
| Starter forced inactive after `MAX_CRANK_MS` | `enforceSafety()` backstop, independent of the state machine |
| Choke never active indefinitely | `MAX_CHOKE_MS` backstop → `CHOKE_OVERRUN` fault |
| Spare relay always inactive | `setSpareRelay()` ignores activation; `enforceSafety()` faults if the flag is ever set |
| START rejected unless state is exactly `IDLE` | `startPermitted()` |
| No automatic restart after reboot | Initial state is `UNKNOWN` |
| No automatic retry after a failed start | `RETRY_WAIT` only ever falls back to `IDLE` |
| State transitions logged | Serial, at 115200, never including secrets |

### Fault handling

If a hard invariant is violated the controller deactivates the starter,
deactivates the choke, and — if the engine could plausibly be running —
grounds the kill circuit for `KILL_HOLD_MS` before releasing it. It then
enters `FAULT` and reports the cause via `/v1/status`:

| Fault | Cause |
| ----- | ----- |
| `STARTER_KILL_CONFLICT` | Starter and kill were both commanded active |
| `STARTER_OVERRUN` | Starter active beyond `MAX_CRANK_MS` (implies the main loop stalled) |
| `CHOKE_OVERRUN` | Choke active beyond `MAX_CHOKE_MS` |
| `SPARE_ACTIVE` | K4 was found active |

`FAULT` is sticky. Clear it with `POST /v1/stop` (which lands in `IDLE`) or
`POST /v1/reset-idle`.

### Non-blocking design

There are no long `delay()` calls anywhere in the control path. Everything is
`millis()`-driven, and every elapsed-time check is rollover-safe:

```cpp
static_cast<uint32_t>(now - startedAt) >= duration
```

The rollover behaviour is explicitly tested — the suite runs full start/stop
sequences with the clock starting at `0xFFFFFF00` so every timer boundary
wraps.

The main loop, in order:

1. Enforce hard relay safety limits
2. Advance the pump state machine
3. Service an existing HTTP client incrementally (≤ 128 bytes per pass)
4. Accept a new HTTP client
5. Perform Wi-Fi maintenance **only when quiescent**
6. Update the LED matrix (cosmetic)
7. Feed the watchdog

HTTP parsing uses bounded fixed-size buffers throughout. No `String`
concatenation, no heap allocation, no unbounded collections.

---

## Watchdog

The firmware uses the **Renesas UNO core's WDT library** (`libraries/WDT`):

```cpp
#include <WDT.h>

int      WDT.begin(uint32_t timeout_ms);  // 1 on success, 0 if unsupported
void     WDT.refresh();
uint32_t WDT.getTimeout();                // period actually programmed
```

Configured in `config.h`:

```cpp
#define ENABLE_WATCHDOG 1
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 5000;
```

* The RA4M1 WDT derives its period from PCLKB, so the core rounds the request
  **up** to the nearest supported prescaler/reload pair. The programmed value
  is printed at boot.
* **Maximum achievable period is roughly 5.6 s** on this board
  (`16384 × 8192 / (PCLKB/1000)`). Requesting more makes `begin()` fail;
  `WATCHDOG_TIMEOUT_MS` must stay below it. If `begin()` fails the firmware
  logs it and continues without a watchdog rather than pretending.
* It is refreshed **only after a complete, successful main-loop iteration**.
* Set `ENABLE_WATCHDOG` to `0` to build without it.

### The WiFiS3 blocking caveat — and why a reset is safe

Every WiFiS3 call is a synchronous AT exchange with the on-board ESP32-S3,
with a **10 second** modem timeout. That is longer than the maximum watchdog
period, so a genuinely hung modem transaction *will* trip the watchdog.

This firmware minimises the exposure and makes the outcome safe:

* `WiFi.setTimeout(0)` removes WiFiS3's own internal 10-second connect poll,
  so `WiFi.begin()` only sends its AT commands and returns. Association
  progress is then polled from the main loop, one quick query per second,
  spread across many iterations.
* A reconnect is only ever *initiated* while the controller is quiescent.
* HTTP servicing does touch the modem on every iteration — it must, so that a
  `STOP` can be received during `CRANKING`.

**If the watchdog does fire mid-crank, the outcome is exactly what you want:**
the reset drives every GPIO to high-impedance, the relay module de-energises,
the starter releases, and the controller comes back in `UNKNOWN` with all
relays inactive and `START` refused. The system is fail-safe by construction
because a relay is only ever energised while firmware is actively driving it.

> This relies on your relay board de-energising when its `IN` pin floats,
> which is normal for optoisolated boards with input pull-ups.
> **Bench test 21 verifies it on your actual hardware.**

---

## Testing

Three independent layers.

### 1. Host unit tests — no hardware required

The state machine, relay layer, HTTP parser, router, authentication and JSON
generation are all compiled and run natively. Only a C++17 compiler is needed
— no board, no network, no Arduino toolchain.

```bash
# Linux / macOS / WSL
./src/arduino/tests/run_tests.sh

# Windows
powershell -ExecutionPolicy Bypass -File src\arduino\tests\run_tests.ps1
```

Current status:

```
tests: 151 passed, 0 failed | assertions: 8668034 run, 0 failed
HOST TEST SUITE PASSED (both polarities x maintenance on/off)
```

The suite is built and run across a **four-way configuration matrix** — both
relay polarities × maintenance API on/off — so `RELAY_ACTIVE_LOW` is proven to
be the single point of control, and `ENABLE_MAINTENANCE_API` is proven to
control endpoint reachability in both directions. Warnings are errors (`-Wall
-Wextra -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror`).

A fake Arduino layer (`tests/shim/`) records every `pinMode` and
`digitalWrite` with a timestamp and sequence number, so tests assert on the
**actual electrical levels and their ordering**, not on internal flags. That
is how "STOP releases the starter *before* grounding kill" is verified.

Coverage highlights:

* Boot: `UNKNOWN`, all relays inactive, and every pin pre-driven to its
  inactive level *before* `pinMode(OUTPUT)`.
* Full start sequence with millisecond-exact boundary checks at
  `CHOKE_PREP_MS`, `CRANK_DURATION_MS` and `UNCHOKE_DELAY_MS`.
* STOP accepted from all nine states, with relay ordering verified.
* Every interlock provoked directly through a test-only accessor, including
  corrupted internal flags that `enforceSafety()` must catch.
* `millis()` rollover across full sequences and the cooldown.
* Idempotency: ring bounds, eviction, command scoping, rejected-command
  handling, and the `STOP` exemption.
* HTTP: malformed request lines, oversized lines/headers, case-insensitive
  header matching, request-ID validation, constant-time comparison, JSON
  shape/escaping/buffer-exhaustion.
* API: `401` before routing, `404`/`405`/`409`/`413`, no secret ever echoed.

### 2. Property-based fuzzing

`tests/test_invariants.cpp` attacks the same code with millions of randomised
command and timing permutations, asserting the safety invariants after **every
single step**:

* starter and kill never energised together (flags *and* pin levels)
* the spare relay is never energised, ever
* after every tick the starter is either released or has been engaged for
  strictly less than `MAX_CRANK_MS`
* commanded pin levels always agree with reported relay state
* `running_confirmed` is never true
* quiescence always implies no relay is energised

It also fuzzes the HTTP parser with random byte soup and mutated valid
requests, asserting that **no request without the exact secret ever produces a
2xx**, that only documented status codes are emitted, and that the parser
never crashes.

A fixed-seed xorshift PRNG makes every failure exactly reproducible from the
seed printed in the assertion message.

> **This layer earned its keep.** The fuzzer found a real bug: a replayed
> `STOP` request ID was being suppressed by duplicate detection, so a stale
> retransmission arriving after a later `START` could be silently swallowed
> while the engine was cranking. That is why `STOP` is now exempt from
> duplicate suppression.

### 3. Hardware-in-the-loop bench test

`tests/hil/bench_test.py` drives a real device over the LAN and automates most
of the checklist below, prompting for the checks that need a human watching
the relay board. Standard library only — no `pip install`.

```bash
python3 src/arduino/tests/hil/bench_test.py \
  --host 192.168.1.50 \
  --secret YOUR_SECRET \
  --assume-fresh-boot
```

Optional interactive extras: `--wifi-tests`, `--reboot-test`.

It measures actual relay timings by polling `/v1/status`, so its tolerance is
±450 ms. Use the Serial log or a scope if you need millisecond accuracy.

> ⚠️ The script issues **real** `START` commands. Run it only with the
> engine's 12 V contact-side wiring disconnected. It always returns the device
> to a safe state on exit.

### 4. Client / integration tests

The reference Pi client is exercised against the API stub — no hardware, no
network beyond loopback:

```bash
cd docs/examples && python3 test_examples.py
```

```
passed 88 | failed 0
```

This covers the boundary the Pi will actually depend on: `START` gating, the
full sequence, `STOP` from every state, the duplicate-suppression rules
(including the `STOP` exemption), `401` before routing, request-ID validation,
timing overrides and their rejection cases, the maintenance API in both the
enabled and disabled builds, unreachable-device handling and the background
poller.

---

## Bench-test procedure

**Perform all initial testing with the engine's 12 V contact-side wiring
disconnected.**

Setup:

* Arduino powered from a normal USB-C wall charger or a computer.
* Relay module connected **only** to Arduino `5V`, `GND` and `D2`–`D5`.
* Nothing on `COM`/`NO`/`NC`.
* Watch the relay LEDs and listen for clicks, or meter across `COM` and `NO`.

| # | Test | Expected |
|---|------|----------|
| 1 | Power-on | No unintended relay activation — no click, no LED |
| 2 | Initial API state | `UNKNOWN` |
| 3 | `POST /v1/start` from `UNKNOWN` | `409`, no relay moves |
| 4 | `POST /v1/reset-idle` | `202`, state becomes `IDLE` |
| 5 | `POST /v1/start` | choke → starter → starter release → unchoke |
| 6 | Starter engagement | matches `CRANK_DURATION_MS` (2 s) |
| 7 | Starter ceiling | never exceeds 5 s under any condition |
| 8 | STOP during `CHOKING` | choke cancels immediately, kill grounds |
| 9 | STOP during `CRANKING` | starter releases **before** kill grounds |
| 10 | STOP from `RUNNING_ASSUMED` | kill grounded for 3 s, then released |
| 11 | START from every non-`IDLE` state | rejected with `409` |
| 12 | Duplicate request IDs | operation not repeated; `"duplicate": true` |
| 13 | Incorrect secrets | `401`, and no relay changes |
| 14 | Wi-Fi disconnect mid-sequence | timing sequence continues unaffected |
| 15 | Wi-Fi returns | device reconnects automatically |
| 16 | Reboot | returns to `UNKNOWN`, not `IDLE` or `RUNNING_ASSUMED` |
| 17 | Serial output | hostname, firmware, SSID, IP, MAC, RSSI, port — no secrets |
| 18 | LAN reachability | another computer can call `/v1/status` |
| 19 | NetScan / router client list | device visible, ideally as `fire-pump-controller` |
| 20 | Malformed and oversized HTTP | cannot leave the starter energised |
| 21 | Reset during `CRANKING` | pressing RESET mid-crank **releases K1** — confirms the fail-safe |

Test 21 is the one that validates the watchdog story above. Do it explicitly:
start a sequence, press the Arduino RESET button while the starter relay is
energised, and confirm K1 drops immediately.

### After the logic tests pass

Before applying any 12 V:

1. Power the Arduino and confirm all relays are inactive.
2. With a multimeter on continuity, verify each relay's `COM`→`NO` reads
   **open** when inactive and **closed** when active, for K1, K2 and K3.
3. Confirm K4 stayed inactive throughout.
4. Confirm your fusing: the inline 5 A fuse near the battery, and appropriate
   fusing on the K1 and K2 `COM` feeds.

### First 12 V test

1. **Do not connect the starter motor's heavy-current cable yet.** Confirm the
   low-current solenoid *control* circuit and its fusing first.
2. Verify the choke actuator direction: K2 active must **engage** choke, and
   K2 inactive must return it to unchoked. Confirm this before connecting
   anything to the engine.
3. Verify the ignition-kill wire with a meter before connecting K3. Confirm
   you have identified the correct wire and that **K3 only ever grounds it** —
   never injects 12 V.
4. Only then connect the starter cable and attempt a real start, with someone
   at the engine and the camera feed up.

---

## Troubleshooting

**Relays click at power-on, or behave inverted**
Your module is active-high. Set `RELAY_ACTIVE_LOW = false` in `config.h` and
re-upload. Re-run the host tests; they cover both polarities.

**Device never appears on the LAN**
Enable the [serial console](#serial-bench-console-disabled-by-default) and run
`scan`. It lists exactly what the radio can see.

* **SSID not in the list** → it is 5 GHz only. The UNO R4 WiFi has no 5 GHz
  radio. Enable a 2.4 GHz SSID on your router.
* **SSID in the list but not connecting** → compare it character for character
  with `arduino_secrets.h`. **SSIDs are case-sensitive**, and this cost real
  time during commissioning: `My Wifi` silently fails against a network
  actually named `My WiFi`. The only symptom is an endless
  `associating… / association timed out` loop with no other clue. Copy the
  SSID out of the `scan` output rather than typing it.

**`[WIFI] associated; waiting for DHCP` then a retry**
The access point accepted the association but no DHCP lease arrived within
`DHCP_TIMEOUT_MS`. Check the router's DHCP pool is not exhausted, and that MAC
filtering is not blocking the device.

**No relay LEDs ever light**
That is correct until something commands a relay. With `RELAY_ACTIVE_LOW`, an
inactive relay means the pin is driven **HIGH**, so `IN1`–`IN4` are dark and
the coils are de-energised. Prove the wiring with the serial console's `test`
command, or watch `IN2`/`IN1` during a `POST /v1/start`.

**`fire-pump-controller.local` does not resolve**
Expected. Setting a DHCP hostname is not mDNS. Use the IP address with a DHCP
reservation.

**Everything returns 401**
Check `PUMP_API_SECRET` in `arduino_secrets.h` matches the `X-Pump-Secret`
header exactly, including case. An empty configured secret fails closed by
design.

**`START` returns 409 from `IDLE`**
The recrank cooldown is still running. Check `cooldown_remaining_ms` in the
409 body or in `/v1/status`.

**State is `FAULT`**
Read the `fault` field in `/v1/status`. `STARTER_OVERRUN` means the main loop
stalled past `MAX_CRANK_MS` during a crank — worth investigating, most likely
a hung modem transaction. Clear with `/v1/stop` or `/v1/reset-idle`.

**Board reboots repeatedly when the AP is down**
A modem transaction is exceeding the watchdog period. Set `ENABLE_WATCHDOG` to
`0` temporarily to confirm, then report it — the resets themselves are safe
(all relays de-energise) but the loop is not.

**The LED matrix animation is distracting**
Set `ENABLE_LED_MATRIX` to `0` in `config.h`.

---

## Project layout

```
remote-firepump/
├── README.md                      ← this document
├── .gitignore
├── docs/
│   ├── PI_INTEGRATION.md          Raspberry Pi integration contract
│   ├── openapi.yaml               OpenAPI 3.1 spec
│   └── examples/
│       ├── pump_client.py         reference Python client
│       ├── stub_server.py         API stub for hardware-free development
│       └── test_examples.py       client-vs-stub self-test
├── pump-gateway/                  the Raspberry Pi side (Go)
│   ├── README.md                  gateway, deployment and Tailscale guide
│   ├── cmd/pump-gateway/          the service: web app + Arduino proxy
│   ├── cmd/mock-arduino/          software stand-in for this firmware
│   ├── internal/                  client, auth, config, monitor, server, events
│   ├── web/                       the embedded PWA (no CDN, no build step)
│   ├── deploy/                    systemd unit, env template, installer
│   └── test/e2e/                  runs the built binaries as processes
└── src/arduino/
    ├── fire_pump_controller/
    │   ├── fire_pump_controller.ino    main loop and setup
    │   ├── config.h                    all tunables, pins, compile-time asserts
    │   ├── pump_controller.{h,cpp}     state machine + relay layer + interlocks
    │   ├── http_protocol.{h,cpp}       HTTP parsing, routing, JSON  (no WiFi)
    │   ├── api_handler.{h,cpp}         auth, dispatch, response planning (no WiFi)
    │   ├── http_server.{h,cpp}         WiFiS3 socket plumbing
    │   ├── net_manager.{h,cpp}         non-blocking Wi-Fi lifecycle
    │   ├── led_matrix.{h,cpp}          cosmetic flame status display
    │   ├── arduino_secrets.example.h   template — copy to arduino_secrets.h
    │   ├── .gitignore                  excludes arduino_secrets.h
    │   └── README.md                   sketch-level quickstart
    └── tests/
        ├── run_tests.sh / run_tests.ps1
        ├── shim/Arduino.{h,cpp}        instrumented fake Arduino core
        ├── test_framework.h            assertions and registration
        ├── test_main.cpp               runner
        ├── test_support.{h,cpp}        fixtures and helpers
        ├── test_pump_controller.cpp    state machine and interlocks
        ├── test_http_protocol.cpp      parsing, routing, JSON
        ├── test_api_handler.cpp        end-to-end API behaviour
        ├── test_invariants.cpp         property-based fuzzing
        └── hil/bench_test.py           hardware-in-the-loop bench test
```

The layout differs slightly from a flat sketch folder: `api_handler` and
`net_manager` are split out from `http_server`. That is deliberate — it keeps
**all** decision logic (state machine, parsing, routing, authentication, JSON)
free of `WiFiS3`, which is what makes the host test suite possible.

### Deliberately not included

No sensors, MQTT, cloud hosting, Tailscale on the Arduino, native iPhone
integration, OTA updates, or automatic retries. This firmware stays focused on
reliable LAN control of the three required relays.

---

## Assumptions requiring physical verification

Everything below is a software assumption that **must be confirmed against
your actual hardware** before the system is trusted with a real engine.

1. **Relay module polarity is active-low.** Verify at power-on (bench test 1).
   If wrong, flip `RELAY_ACTIVE_LOW`.
2. **The relay module de-energises when its `IN` pin floats.** The entire
   power-loss and watchdog-reset fail-safe story depends on this. Verify with
   bench test 21.
3. **The `VCC`/`JD-VCC` jumper is installed** and the Arduino's 5 V rail can
   supply the coil current for the number of relays energised at once. If your
   board draws more than the rail can provide, power the coils separately and
   remove the jumper.
4. **The choke actuator engages choke when K2 is active** and returns to
   unchoked when K2 is inactive. Verify on the bench before connecting to the
   engine.
5. **The Honda ignition-kill wire has been correctly identified**, and
   grounding it stops the engine. Verify with a meter.
6. **The starter-solenoid trigger circuit is the low-current `S` control
   circuit**, not the heavy motor feed, and K1's contact rating is adequate
   for it.
7. **Wire colours and terminal identification** must come from your specific
   Honda electric-start harness. None are invented in this document.
8. **`CHOKE_PREP_MS` and `CRANK_DURATION_MS` suit your engine.** Two seconds
   of cranking is a starting point, not a tuned value. Adjust in `config.h`
   after observing real starts.
9. **The DHCP reservation is configured** so the Pi's address never changes.
10. **Port 8080 is not reachable from outside the LAN.** Confirm from an
    external network.
