// config.h — compile-time configuration for the fire pump controller.
//
// Everything an installer might reasonably want to change lives here.
// Wiring-critical values are cross-checked with static_assert where possible.

#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// 0.2.0 assigned K4 to the priming valve. That is a breaking API change:
// relay_outputs."spare" became relay_outputs."valve", and two states were
// added (PRIMING, VALVE_CLOSING).
constexpr char FIRMWARE_VERSION[] = "0.2.0";

// Requested DHCP hostname. This is what the router *may* show in its client
// list. It does NOT guarantee mDNS / "fire-pump-controller.local" resolution.
constexpr char DEVICE_HOSTNAME[] = "fire-pump-controller";

// ---------------------------------------------------------------------------
// Relay module electrical polarity
// ---------------------------------------------------------------------------
//
// Most cheap 4-channel optoisolated relay boards are ACTIVE LOW: driving the
// IN pin LOW energizes the coil. If your board behaves the opposite way
// (relay clicks when the pin is HIGH), set this to false and re-upload.
//
// This is the ONLY place polarity is encoded. Every relay operation in the
// firmware goes through setRelayOutput() in pump_controller.cpp.
//
// RELAY_ACTIVE_LOW_OVERRIDE exists purely so the host unit tests can build the
// whole state machine a second time at the opposite polarity. Leave it unset
// for real firmware and edit the literal below instead.
#ifdef RELAY_ACTIVE_LOW_OVERRIDE
constexpr bool RELAY_ACTIVE_LOW = RELAY_ACTIVE_LOW_OVERRIDE;
#else
constexpr bool RELAY_ACTIVE_LOW = true;
#endif

// ---------------------------------------------------------------------------
// Pin map — must match the wiring table in README.md
// ---------------------------------------------------------------------------
//
//   Arduino 5V  -> relay VCC
//   Arduino GND -> relay GND
//   D2 -> IN1  K1 starter command
//   D3 -> IN2  K2 choke command
//   D4 -> IN3  K3 RUN-ENABLE  (wired to the NC contact -- see below)
//   D5 -> IN4  K4 intake valve command
//   D6 <- water-available interlock input (switch to GND)
//
// D0/D1 are deliberately avoided (RX/TX). D3 and D5 are PWM-capable but are
// used strictly as plain digital outputs; no analogWrite() anywhere.

constexpr uint8_t PIN_RELAY_STARTER = 2;  // IN1 / K1
constexpr uint8_t PIN_RELAY_CHOKE   = 3;  // IN2 / K2
constexpr uint8_t PIN_RELAY_KILL    = 4;  // IN3 / K3 run-enable
constexpr uint8_t PIN_RELAY_VALVE   = 5;  // IN4 / K4 intake valve
constexpr uint8_t PIN_WATER_OK      = 6;  // input, water-available interlock

// ---------------------------------------------------------------------------
// K3 is a FAIL-SAFE RUN-ENABLE relay, not a "kill" relay
// ---------------------------------------------------------------------------
//
// A GX390's magneto ignition is independent of the battery and the electric
// starter: cutting controller power does NOT stop the engine. Combined with a
// fail-closed intake valve, a naive "energise K3 to stop" design produces the
// worst possible outcome on power loss -- valve shuts, engine keeps running,
// pump runs dry, mechanical seal destroyed.
//
// K3 is therefore wired to its NORMALLY CLOSED contact and must be held
// ENERGISED for the engine to be permitted to run:
//
//   K3 COM -> verified Honda ignition-kill wire
//   K3 NC  -> engine ground
//   K3 NO  -> unused
//
//   relay DE-ENERGISED -> NC closed -> kill wire grounded -> engine CANNOT run
//   relay ENERGISED    -> NC open   -> kill wire floating -> engine MAY run
//
// So controller power loss, a watchdog reset, a blown 5 V rail or a snapped
// IN3 wire all ground the kill wire and stop the engine. That is the point.
//
// Internally the firmware still reasons in terms of "kill asserted" (engine
// prevented from running). The inversion lives in exactly one place --
// setKillRelay() in pump_controller.cpp -- alongside RELAY_ACTIVE_LOW.
//
// Set to false ONLY if you deliberately wired K3 to NO instead, which gives
// up the fail-safe. The firmware will warn loudly at boot.
constexpr bool KILL_RELAY_FAIL_SAFE_NC = true;

static_assert(PIN_RELAY_STARTER != 0 && PIN_RELAY_STARTER != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_CHOKE   != 0 && PIN_RELAY_CHOKE   != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_KILL    != 0 && PIN_RELAY_KILL    != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_VALVE   != 0 && PIN_RELAY_VALVE   != 1, "D0/D1 reserved for RX/TX");

static_assert(PIN_RELAY_STARTER != PIN_RELAY_CHOKE, "relay pins must be distinct");
static_assert(PIN_RELAY_STARTER != PIN_RELAY_KILL,  "relay pins must be distinct");
static_assert(PIN_RELAY_STARTER != PIN_RELAY_VALVE, "relay pins must be distinct");
static_assert(PIN_RELAY_CHOKE   != PIN_RELAY_KILL,  "relay pins must be distinct");
static_assert(PIN_RELAY_CHOKE   != PIN_RELAY_VALVE, "relay pins must be distinct");
static_assert(PIN_RELAY_KILL    != PIN_RELAY_VALVE, "relay pins must be distinct");

// ---------------------------------------------------------------------------
// Sequence timings (milliseconds)
// ---------------------------------------------------------------------------

// Choke engaged before the starter is engaged.
constexpr uint32_t CHOKE_PREP_MS = 1000;

// Nominal starter engagement time for one start attempt.
constexpr uint32_t CRANK_DURATION_MS = 2000;

// Absolute hard ceiling on starter engagement, enforced at the relay layer
// independently of the state machine. The starter can never be commanded
// active for longer than this, on any path, normal or fault.
constexpr uint32_t MAX_CRANK_MS = 5000;

// Delay from starter release to choke release.
constexpr uint32_t UNCHOKE_DELAY_MS = 500;

// How long the kill circuit is held to ground during a STOP.
constexpr uint32_t KILL_HOLD_MS = 3000;

// Minimum gap between starter engagements. Protects the starter motor and
// solenoid from rapid re-cranking.
constexpr uint32_t MIN_RECRANK_GAP_MS = 10000;

// Backstop: choke may never remain commanded active longer than this. Under
// normal operation the state machine releases it far sooner.
constexpr uint32_t MAX_CHOKE_MS = 30000;

// ---------------------------------------------------------------------------
// Intake valve (K4) -- OPTIONAL
// ---------------------------------------------------------------------------
//
// A 2" normally-closed 12 V solenoid valve on the pump INTAKE. De-energised =
// CLOSED, so it shuts at power-on and on any power loss or reset.
//
// THINK HARD BEFORE ENABLING THIS.
//
// An electrically operated intake valve is the single most dangerous part of
// this design. If it closes while the engine runs, the pump loses water and
// runs dry; Honda warn that extended dry running destroys the mechanical
// seal. Software cannot protect against a broken valve wire, a blown valve
// fuse, or a mechanically stuck valve -- by the time the firmware could
// notice, the seal is already going.
//
// If the tank line can safely stay pressurised, the simpler and more reliable
// answer is to DELETE the electric valve: leave a manual intake valve open
// permanently and fit a foot/check valve to retain prime. Then set this to 0,
// K4 goes unused, and a whole class of failure disappears.
//
// If you do enable it, read ACKNOWLEDGE_NO_WATER_INTERLOCK below. A water
// interlock is strongly preferred; running without one is permitted only as
// an explicit, acknowledged trade-off.
#ifndef ENABLE_INTAKE_VALVE
#define ENABLE_INTAKE_VALVE 1
#endif

constexpr bool INTAKE_VALVE_ENABLED = (ENABLE_INTAKE_VALVE != 0);

// How long the valve is held open to prime before the choke/crank sequence
// begins. Tune to your suction lift and hose length.
constexpr uint32_t VALVE_PRIME_MS = 5000;

// After the kill circuit is grounded, how long to wait before closing the
// valve. Gives the engine time to actually come to rest, so the intake is
// never shut on a still-spinning pump.
constexpr uint32_t VALVE_CLOSE_DELAY_MS = 3000;

static_assert(VALVE_PRIME_MS > 0, "priming with a zero-length open is pointless");
static_assert(VALVE_CLOSE_DELAY_MS > 0,
              "the engine needs time to stop before the intake is shut");

// ---------------------------------------------------------------------------
// Water-available interlock (D6)
// ---------------------------------------------------------------------------
//
// A pressure or flow switch on the intake, reporting that water is actually
// present at the pump.
//
// THE PRIMARY PROTECTION IS HARDWIRED, NOT THIS INPUT. Wire the switch so it
// grounds the ignition-kill wire directly -- in series with / parallel to K3's
// NC contact -- so that losing water stops the engine even if this Arduino is
// dead, hung, or has been unplugged. This firmware input is a SECOND,
// independent layer: it lets the controller refuse to start, report the
// condition over the API, and shut down in an orderly way.
//
// Wiring: switch closes to GND when water IS available. The pin uses
// INPUT_PULLUP, so a broken wire or disconnected sensor reads HIGH = "no
// water" = refuse to run. Failure of the sensor wiring is therefore safe.
constexpr uint8_t WATER_OK_LEVEL = LOW;

// Debounce: the reading must be stable this long before it is believed.
// Pressure switches chatter around their setpoint, and a fire pump must not
// shut down on a momentary dip.
constexpr uint32_t WATER_DEBOUNCE_MS = 750;

// Grace period after the starter is released before loss of water is acted
// on, so a pump that has not yet built pressure is not immediately killed.
constexpr uint32_t WATER_STARTUP_GRACE_MS = 15000;

// When 0, the firmware has no water sensor and treats water as always
// available. Prime then becomes purely time-based: the valve is opened and the
// dwell observed, and the firmware trusts that an open valve means water.
#ifndef REQUIRE_WATER_INTERLOCK
#define REQUIRE_WATER_INTERLOCK 0
#endif

constexpr bool WATER_INTERLOCK_REQUIRED = (REQUIRE_WATER_INTERLOCK != 0);

// ---------------------------------------------------------------------------
// Running an intake valve with no water sensor
// ---------------------------------------------------------------------------
//
// This combination is a DELIBERATE, ACKNOWLEDGED trade-off on this install.
// It must stay deliberate, so it has to be acknowledged explicitly rather
// than arrived at by editing one flag.
//
// What is given up: the firmware can no longer tell the difference between
// "valve commanded open, water flowing" and "valve commanded open, but the
// actuator is stuck, the fuse is blown, the lead is broken, or the source is
// dry". In every one of those the pump runs without water and the mechanical
// seal is damaged.
//
// What still works, because it does not depend on sensing water:
//   * the starter is refused unless the valve is commanded open and the prime
//     dwell has elapsed;
//   * the engine is shut down if the valve is ever commanded shut while it
//     could be turning;
//   * K3's NC contact still grounds the ignition on power loss or reset.
//
// The operator is the sensor: the camera confirms the engine caught and that
// water is moving. That is a one-off human check, not a continuous one, so
// this configuration relies on the pump not being left running unattended.
#ifndef ACKNOWLEDGE_NO_WATER_INTERLOCK
#define ACKNOWLEDGE_NO_WATER_INTERLOCK 1
#endif

static_assert(!INTAKE_VALVE_ENABLED || WATER_INTERLOCK_REQUIRED ||
                  ACKNOWLEDGE_NO_WATER_INTERLOCK,
              "An electrically operated intake valve without a water "
              "interlock can run the pump dry and destroy the seal. Fit the "
              "interlock (REQUIRE_WATER_INTERLOCK=1), delete the electric "
              "valve (ENABLE_INTAKE_VALVE=0), or acknowledge the risk "
              "explicitly (ACKNOWLEDGE_NO_WATER_INTERLOCK=1).");


// ---------------------------------------------------------------------------
// DANGER_OVERRIDE
// ---------------------------------------------------------------------------
//
// A command carrying the X-Danger-Override header is forced past the
// PRECONDITION interlocks: not being IDLE, an unexpired recrank cooldown, an
// incomplete prime dwell, relays not at rest, absent water.
//
// It does NOT relax the destructive-limit backstops -- MAX_CRANK_MS, the
// starter/kill exclusion, shutdown on a commanded-shut intake, or the
// fail-safe kill on reset. Those are not judgement calls.
//
// The value must match this phrase EXACTLY. A bare "true" or "1" is rejected
// with 400 on purpose: a generic HTTP client, a proxy adding defaults, or a
// copied curl line should not be able to arm this by accident. Typing the
// phrase is the point.
constexpr char DANGER_OVERRIDE_TOKEN[] = "I-ACCEPT-THE-RISK";

// ---------------------------------------------------------------------------
// Per-request timing overrides
// ---------------------------------------------------------------------------
//
// POST /v1/start accepts optional X-Choke-Ms / X-Crank-Ms / X-Unchoke-Ms
// headers. Omitted headers fall back to the defaults above.
//
// These are upper bounds on what a caller may request. A request above the
// bound is rejected with 400 rather than silently clamped, so the caller
// always knows what actually ran. MAX_CRANK_MS remains enforced independently
// at the relay layer regardless of what any request asks for.
constexpr uint32_t MAX_CHOKE_PREP_OVERRIDE_MS    = 15000;
constexpr uint32_t MAX_UNCHOKE_DELAY_OVERRIDE_MS = 5000;

// The worst-case requestable sequence must still finish inside the choke
// backstop, or a legal request could fault itself.
static_assert(MAX_CHOKE_PREP_OVERRIDE_MS + MAX_CRANK_MS +
                      MAX_UNCHOKE_DELAY_OVERRIDE_MS < MAX_CHOKE_MS,
              "the longest requestable start sequence must fit inside MAX_CHOKE_MS");
static_assert(CHOKE_PREP_MS <= MAX_CHOKE_PREP_OVERRIDE_MS,
              "the default choke prep must be requestable");
static_assert(UNCHOKE_DELAY_MS <= MAX_UNCHOKE_DELAY_OVERRIDE_MS,
              "the default unchoke delay must be requestable");

// ---------------------------------------------------------------------------
// Maintenance relay API
// ---------------------------------------------------------------------------
//
// When enabled, exposes POST /v1/maintenance/{choke,starter,kill}/{on,off}
// for bench commissioning. OFF BY DEFAULT: direct relay control bypasses the
// start/stop sequencing that makes the normal API safe.
//
// Even when enabled, every hard interlock still applies -- the starter and
// kill can never be energised together, the starter is still force-released
// at MAX_CRANK_MS, and the choke is still force-released at MAX_CHOKE_MS.
// Manual commands are only accepted from IDLE or UNKNOWN.
//
// Do not ship this enabled on a controller wired to a real engine.
#ifndef ENABLE_MAINTENANCE_API
#define ENABLE_MAINTENANCE_API 0
#endif

constexpr bool MAINTENANCE_API_ENABLED = (ENABLE_MAINTENANCE_API != 0);

// ---------------------------------------------------------------------------
// Serial bench console
// ---------------------------------------------------------------------------
//
// A commissioning console over the USB cable, for proving the Arduino-to-relay
// wiring before Wi-Fi is configured (see serial_console.h). Includes a lamp
// test that pulses each relay in turn.
//
// OFF BY DEFAULT. It drives real relays, so turn it off before the controller
// goes on the engine. Every interlock still applies while it is on.
// NOTE: currently ENABLED for bench commissioning. Set back to 0 before the
// controller goes on the engine.
#ifndef ENABLE_SERIAL_CONSOLE
#define ENABLE_SERIAL_CONSOLE 1
#endif

// Longest console command line accepted; longer lines are discarded.
constexpr size_t SERIAL_LINE_MAX = 48;

// Bytes drained from the USB serial buffer per main-loop pass, so a paste of
// text cannot stall the state machine.
constexpr size_t SERIAL_BYTES_PER_PASS = 32;

// How long the lamp test energises each relay. Well under MAX_CRANK_MS, which
// still applies to the starter pulse regardless.
constexpr uint32_t LAMP_TEST_PULSE_MS = 600;

// Gap between lamp-test pulses, so each click is distinguishable.
constexpr uint32_t LAMP_TEST_GAP_MS = 400;

static_assert(LAMP_TEST_PULSE_MS < MAX_CRANK_MS,
              "a lamp-test starter pulse must stay inside the starter ceiling");

static_assert(CRANK_DURATION_MS <= MAX_CRANK_MS,
              "CRANK_DURATION_MS must not exceed MAX_CRANK_MS");
static_assert(MAX_CRANK_MS <= 5000,
              "MAX_CRANK_MS must not exceed 5 seconds of starter engagement");
static_assert(CHOKE_PREP_MS + MAX_CRANK_MS + UNCHOKE_DELAY_MS < MAX_CHOKE_MS,
              "MAX_CHOKE_MS must be long enough to cover a full start sequence");
static_assert(MIN_RECRANK_GAP_MS > 0, "a recrank gap is required");

// ---------------------------------------------------------------------------
// Network / HTTP
// ---------------------------------------------------------------------------

constexpr uint16_t HTTP_PORT = 8080;

constexpr unsigned long SERIAL_BAUD = 115200;

// Wait this long for Serial at boot, then continue regardless. The controller
// must never depend on a USB host being attached.
constexpr uint32_t SERIAL_WAIT_MS = 1500;

// A single blocking-ish WiFi.begin() attempt is bounded by this. Reconnection
// is only ever attempted while the state machine is quiescent.
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000;

// Minimum spacing between reconnect attempts.
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;

// How often link status is polled.
constexpr uint32_t WIFI_POLL_INTERVAL_MS = 1000;

// After association, how long to wait for DHCP to bind an address before
// giving up and retrying. WL_CONNECTED arrives well before the lease does;
// announcing at that moment would advertise 0.0.0.0 as the device address.
constexpr uint32_t DHCP_TIMEOUT_MS = 15000;

// HTTP request limits. Anything larger is rejected with 413.
constexpr size_t   HTTP_MAX_REQUEST_LINE = 256;   // "POST /v1/start HTTP/1.1"
constexpr size_t   HTTP_MAX_HEADER_BYTES = 2048;  // total header section
// NOTE: keep HTTP_BODY_MAX (http_protocol.h) ahead of the worst-case status
// document; test `status_json_fits_the_response_buffer_at_maximum_size`
// fails loudly if a new field ever pushes past it.
constexpr size_t   HTTP_MAX_LINE_BYTES   = 256;   // any single header line

// A client that cannot get its headers in within this window is dropped.
constexpr uint32_t HTTP_CLIENT_TIMEOUT_MS = 3000;

// Minimum spacing between accept polls. WiFiServer::available() is a full AT
// round-trip to the ESP32, and the main loop runs thousands of times a
// second, so polling it every iteration keeps the modem link permanently
// busy for no benefit. Ten milliseconds is invisible next to the ~150 ms a
// request already takes, and cuts modem traffic by orders of magnitude --
// which matters because a single blocked AT call is the one thing that can
// approach the watchdog period.
constexpr uint32_t HTTP_ACCEPT_POLL_MS = 10;

// Request-ID constraints.
constexpr size_t   REQUEST_ID_MAX_LEN = 64;

// When true, a state-changing request without X-Request-ID is rejected with
// 400. The default is false: a missing ID is accepted but forfeits duplicate
// suppression, so callers that want idempotency must supply one. Set to true
// if you would rather fail closed.
constexpr bool     REQUIRE_REQUEST_ID = false;

// Upper bound on bytes drained from one client per main-loop pass. Keeps the
// loop responsive so the state machine keeps ticking while a client is mid
// request.
constexpr size_t   HTTP_BYTES_PER_PASS = 128;

// Idempotency ring buffer depth.
constexpr uint8_t  IDEMPOTENCY_SLOTS = 8;

// ---------------------------------------------------------------------------
// Event log
// ---------------------------------------------------------------------------
//
// A single ring, read non-destructively with a cursor. That gives both
// properties at once: entries survive until overwritten (so nothing is lost
// while the Pi is away), and the Pi "flushes" simply by advancing `since`.
//
// A separate flush-on-read buffer would cost twice the RAM and be less safe:
// destructive reads lose data if the Pi dies between receiving a batch and
// writing it to disk. With a cursor the read is idempotent -- re-requesting
// the same `since` returns the same entries.
//
// 128 x 12 bytes = 1.5 KB. A full start/stop cycle emits roughly a dozen
// entries, so at any sane poll interval the ring never wraps between drains.
constexpr size_t   LOG_RING_ENTRIES = 128;

// Entries per response. Bounded so one drain cannot monopolise the loop or
// overflow the response buffer; the Pi pages with the returned `next`.
constexpr size_t   LOG_MAX_PER_RESPONSE = 20;

// Response buffer for /v1/log. Entries are emitted as compact tuples rather
// than objects -- about 34 bytes each instead of ~110.
constexpr size_t   LOG_BODY_MAX = 1536;

// ---------------------------------------------------------------------------
// LED matrix status display (cosmetic)
// ---------------------------------------------------------------------------
//
// Replaces the board's factory "heart" animation with a dancing flame whose
// height tracks the controller state. Set to 0 to leave the matrix dark.
// See led_matrix.h; it does not touch D2-D5 or any relay line.
#ifndef ENABLE_LED_MATRIX
#define ENABLE_LED_MATRIX 1
#endif

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------
//
// The Renesas UNO core ships an official WDT wrapper (WDT.begin/refresh).
// Set to 0 to build without it; see README "Watchdog" for the limitation note.
#ifndef ENABLE_WATCHDOG
#define ENABLE_WATCHDOG 1
#endif

// Requested watchdog period in milliseconds. The RA4M1 WDT is driven from a
// divided peripheral clock, so the core rounds this up to the nearest
// supported period. Must be long enough to cover one bounded Wi-Fi connect
// poll iteration but short enough to recover from a firmware deadlock.
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 5000;

static_assert(WATCHDOG_TIMEOUT_MS > WIFI_POLL_INTERVAL_MS,
              "watchdog must outlast a single Wi-Fi poll iteration");
