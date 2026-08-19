# Wiring — Arduino → relay board

One page. Authoritative source is `fire_pump_controller/config.h`; if the two
ever disagree, config.h wins.

## Signal pins

| Arduino | Relay | Ch | Controls              | Active |
| ------- | ----- | -- | --------------------- | ------ |
| **D2**  | IN1   | K1 | Starter solenoid      | pulsed, max 5 s |
| **D3**  | IN2   | K2 | Choke actuator        | pulsed |
| **D4**  | IN3   | K3 | **Run-enable** (kill) | held while running |
| **D5**  | IN4   | K4 | Intake valve (12 V NC)| held while primed |
| **D6**  | —     | —  | Water-OK input        | input, see below |
| **5V**  | VCC   |    |                       | |
| **GND** | GND   |    | common ground         | |

Relay board is **active-LOW** (`RELAY_ACTIVE_LOW = true`): the firmware drives
a pin LOW to energise that channel. D0/D1 are never used — they are RX/TX.

## Contact side (the 12 V half)

Dry contacts only. **No 12 V ever touches an Arduino pin.**

| Ch | COM                     | NO              | NC             |
| -- | ----------------------- | --------------- | -------------- |
| K1 | starter solenoid trigger| solenoid feed   | unused         |
| K2 | choke actuator          | actuator feed   | unused         |
| K3 | ignition-kill wire      | *unused*        | **engine ground** |
| K4 | valve + lead            | valve feed      | unused         |

### K3 is backwards on purpose

K3 uses **NC**, and is **energised to permit running**:

```
de-energised → NC closed → kill wire grounded → engine CANNOT run   ← default
energised    → NC open   → kill wire floating → engine MAY run
```

A GX390's magneto ignition does not need the battery, so cutting controller
power does **not** stop it. Wiring K3 this way means power loss, a watchdog
reset, a dead 5 V rail or a snapped IN3 wire all ground the kill wire.

> **If K3 is on NO, this firmware is unsafe to run on a live engine.**
> Either move the wire to NC, or set `KILL_RELAY_FAIL_SAFE_NC = false`
> and accept losing the fail-safe.

## D6 — water interlock

`INPUT_PULLUP`, and **LOW means water is present**. A broken wire or an absent
sensor floats HIGH = "no water" = start refused. Fails safe by construction.

This is the *second* layer. The primary protection is a **hardwired** pressure
or flow switch that grounds the kill wire directly, with no firmware involved —
software cannot save you from a stuck valve or a blown valve fuse.

## Checking it without an engine

Leave the 12 V contact side disconnected, then over USB at 115200 baud:

```
test                 lamp-test every channel in the real interlock order
starter on|off       drive one channel (refused unless IDLE/UNKNOWN)
status               show pin states
```

Requires `ENABLE_SERIAL_CONSOLE 1`. **Set it back to 0 before the controller
goes on the pump** — a stray keystroke cranks a live engine.
