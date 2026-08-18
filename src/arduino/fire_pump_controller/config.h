// config.h — compile-time configuration for the fire pump controller.
//
// Everything an installer might reasonably want to change lives here.
// Wiring-critical values are cross-checked with static_assert where possible.

#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

constexpr char FIRMWARE_VERSION[] = "0.1.0";

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
//   D4 -> IN3  K3 stop/kill command
//   D5 -> IN4  K4 spare, permanently inactive
//
// D0/D1 are deliberately avoided (RX/TX). D3 and D5 are PWM-capable but are
// used strictly as plain digital outputs; no analogWrite() anywhere.

constexpr uint8_t PIN_RELAY_STARTER = 2;  // IN1 / K1
constexpr uint8_t PIN_RELAY_CHOKE   = 3;  // IN2 / K2
constexpr uint8_t PIN_RELAY_KILL    = 4;  // IN3 / K3
constexpr uint8_t PIN_RELAY_SPARE   = 5;  // IN4 / K4

static_assert(PIN_RELAY_STARTER != 0 && PIN_RELAY_STARTER != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_CHOKE   != 0 && PIN_RELAY_CHOKE   != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_KILL    != 0 && PIN_RELAY_KILL    != 1, "D0/D1 reserved for RX/TX");
static_assert(PIN_RELAY_SPARE   != 0 && PIN_RELAY_SPARE   != 1, "D0/D1 reserved for RX/TX");

static_assert(PIN_RELAY_STARTER != PIN_RELAY_CHOKE, "relay pins must be distinct");
static_assert(PIN_RELAY_STARTER != PIN_RELAY_KILL,  "relay pins must be distinct");
static_assert(PIN_RELAY_STARTER != PIN_RELAY_SPARE, "relay pins must be distinct");
static_assert(PIN_RELAY_CHOKE   != PIN_RELAY_KILL,  "relay pins must be distinct");
static_assert(PIN_RELAY_CHOKE   != PIN_RELAY_SPARE, "relay pins must be distinct");
static_assert(PIN_RELAY_KILL    != PIN_RELAY_SPARE, "relay pins must be distinct");

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

// HTTP request limits. Anything larger is rejected with 413.
constexpr size_t   HTTP_MAX_REQUEST_LINE = 256;   // "POST /v1/start HTTP/1.1"
constexpr size_t   HTTP_MAX_HEADER_BYTES = 2048;  // total header section
// NOTE: keep HTTP_BODY_MAX (http_protocol.h) ahead of the worst-case status
// document; test `status_json_fits_the_response_buffer_at_maximum_size`
// fails loudly if a new field ever pushes past it.
constexpr size_t   HTTP_MAX_LINE_BYTES   = 256;   // any single header line

// A client that cannot get its headers in within this window is dropped.
constexpr uint32_t HTTP_CLIENT_TIMEOUT_MS = 3000;

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
