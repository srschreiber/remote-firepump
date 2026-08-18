# fire_pump_controller (Arduino sketch)

Firmware for the Arduino UNO R4 WiFi that controls the Honda GX390 pump's
starter, choke and ignition-kill relays.

**The full documentation — wiring, power, API reference, state machine, safety
interlocks and the bench-test procedure — is in the repository root
[`README.md`](../../../README.md).** This file is only a quickstart.

---

## Quickstart

```bash
# 1. Board package
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno

# 2. Credentials (git-ignored)
cp arduino_secrets.example.h arduino_secrets.h
$EDITOR arduino_secrets.h

# 3. Compile
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi .

# 4. Upload
arduino-cli board list
arduino-cli upload --fqbn arduino:renesas_uno:unor4wifi --port COM5 .

# 5. Watch it come up
arduino-cli monitor --port COM5 --config baudrate=115200
```

`arduino_secrets.h` must never be committed. It is excluded by the
`.gitignore` in this directory.

---

## Pin map

| Arduino | Relay | Function |
| ------- | ----- | -------- |
| `5V`    | `VCC` | Relay control power |
| `GND`   | `GND` | Shared control ground |
| `D2`    | `IN1` | K1 starter |
| `D3`    | `IN2` | K2 choke |
| `D4`    | `IN3` | K3 stop/kill |
| `D5`    | `IN4` | K4 spare — permanently inactive |

`D0`/`D1` are reserved for RX/TX. `D3` and `D5` are used as plain digital
outputs only; there is no PWM anywhere.

**Never connect 12 V to any Arduino pin.** The relay `COM`/`NO`/`NC`
terminals are dry contacts, electrically separate from the Arduino.

---

## Files

| File | Role |
| ---- | ---- |
| `fire_pump_controller.ino` | `setup()` and the main loop |
| `config.h` | Every tunable: pins, timings, limits, compile-time assertions |
| `pump_controller.{h,cpp}` | State machine, relay layer, safety interlocks |
| `http_protocol.{h,cpp}` | HTTP parsing, routing, JSON — **no WiFi** |
| `api_handler.{h,cpp}` | Auth, dispatch, response planning — **no WiFi** |
| `http_server.{h,cpp}` | WiFiS3 socket plumbing |
| `net_manager.{h,cpp}` | Non-blocking Wi-Fi lifecycle |
| `led_matrix.{h,cpp}` | Cosmetic flame status display |
| `arduino_secrets.example.h` | Template — copy to `arduino_secrets.h` |

The decision logic is deliberately kept free of `WiFiS3` so it can be compiled
and tested natively on a host. See [`../tests/`](../tests/).

---

## Common changes

Everything below lives in `config.h`.

**Relay module behaves inverted** (clicks when it should release):

```cpp
constexpr bool RELAY_ACTIVE_LOW = false;
```

**Tune the start sequence for your engine:**

```cpp
constexpr uint32_t CHOKE_PREP_MS      = 1000;   // choke before cranking
constexpr uint32_t CRANK_DURATION_MS  = 2000;   // starter engagement
constexpr uint32_t UNCHOKE_DELAY_MS   = 500;    // starter release → choke off
constexpr uint32_t KILL_HOLD_MS       = 3000;   // kill circuit hold
constexpr uint32_t MIN_RECRANK_GAP_MS = 10000;  // minimum gap between cranks
```

`MAX_CRANK_MS` (5000) is the absolute starter ceiling and is enforced
independently of the state machine. `static_assert`s reject a configuration
where `CRANK_DURATION_MS > MAX_CRANK_MS`.

**Turn off the LED matrix flame:**

```cpp
#define ENABLE_LED_MATRIX 0
```

**Build without the watchdog:**

```cpp
#define ENABLE_WATCHDOG 0
```

---

## Before you touch anything

Run the host test suite — it needs no hardware and takes a few seconds:

```bash
../tests/run_tests.sh                                    # Linux/macOS/WSL
powershell -ExecutionPolicy Bypass -File ..\tests\run_tests.ps1   # Windows
```

It builds and runs the whole suite at **both** relay polarities. If a change
breaks a safety invariant, it will tell you before the engine does.
