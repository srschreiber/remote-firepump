// fire_pump_controller.ino
//
// Remote control of the electric start, choke and ignition-kill circuits of a
// Honda GX390-powered water pump, for an Arduino UNO R4 WiFi (ABX00087).
//
// The Arduino owns all relay timing and every safety interlock. The Raspberry
// Pi on the LAN sends only high-level commands (START / STOP / start-failed /
// reset-idle) over an unencrypted, secret-authenticated HTTP/JSON API.
//
// See README.md for wiring, bench-test procedure and the API contract.
//
// SAFETY NOTES
//   * Never connect 12 V to any Arduino GPIO, USB or 5 V pin.
//   * The relay COM/NO/NC contacts are dry contacts, electrically separate
//     from the Arduino. Only IN1..IN4, 5V and GND touch the Arduino.
//   * Loss of Arduino power de-energises every relay. That is the intended
//     fail-safe: no relay is held active without the firmware driving it.
//   * After any reset the controller reports UNKNOWN and refuses START until
//     an operator confirms the engine is stopped via /v1/reset-idle.

#include <Arduino.h>

#include "api_handler.h"
#include "battery.h"
#include "config.h"
#include "fault_handler.h"
#include "http_server.h"
#include "led_matrix.h"
#include "net_manager.h"
#include "pump_controller.h"
#include "serial_console.h"
#include "tank_level.h"

#if ENABLE_WATCHDOG
#include <WDT.h>
#endif

namespace {

PumpController g_pump;
NetManager     g_net;
HttpServer     g_http;
StatusMatrix   g_matrix;
SerialConsole  g_console;


#if ENABLE_WATCHDOG
bool     g_watchdogArmed = false;
#endif

uint32_t g_lastHeartbeatAt = 0;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000;

void setupWatchdog() {
#if ENABLE_WATCHDOG
  // Renesas UNO core watchdog API (libraries/WDT):
  //   int  WDT.begin(uint32_t timeout_ms)  -> 1 on success, 0 if unsupported
  //   void WDT.refresh()
  //   uint32_t WDT.getTimeout()            -> the period actually programmed
  //
  // The RA4M1 WDT derives its period from PCLKB, so the core rounds the
  // request up to the nearest supported prescaler/reload pair. The maximum
  // achievable period on this board is roughly 5.6 s; requesting more makes
  // begin() fail, so WATCHDOG_TIMEOUT_MS must stay under that.
  if (WDT.begin(WATCHDOG_TIMEOUT_MS)) {
    g_watchdogArmed = true;
    Serial.print(F("[WDT] armed, period "));
    Serial.print(WDT.getTimeout());
    Serial.println(F(" ms"));
  } else {
    g_watchdogArmed = false;
    Serial.println(F("[WDT] unavailable; continuing without a watchdog"));
  }
#else
  Serial.println(F("[WDT] disabled at compile time (ENABLE_WATCHDOG=0)"));
#endif
}

inline void feedWatchdog() {
#if ENABLE_WATCHDOG
  if (g_watchdogArmed) {
    WDT.refresh();
  }
#endif
}

void printHeartbeat(uint32_t now) {
  // Bounded, low-rate diagnostics. Nothing here can grow without limit and
  // no secret is ever printed.
  Serial.print(F("[HB] state="));
  Serial.print(toString(g_pump.state()));
  Serial.print(F(" relays s/c/k="));
  Serial.print(g_pump.starterActive() ? 1 : 0);
  Serial.print(g_pump.chokeActive() ? 1 : 0);
  Serial.print(g_pump.killActive() ? 1 : 0);
  Serial.print(F(" wifi="));
  Serial.print(g_net.isConnected() ? 1 : 0);
  Serial.print(F(" ip="));
  Serial.print(g_net.ipString());
  Serial.print(F(" uptime_ms="));
  Serial.println(now);
}

}  // namespace

void setup() {
  // Before anything else: find out why we are here, and make integer
  // divide-by-zero a detectable fault rather than silently wrong arithmetic.
  const ResetReason resetReason = consumeResetReason();
  noteBoot();
  enableDivideByZeroTrap();

  // ---------------------------------------------------------------------
  // 1. Relays first, before anything else can take time.
  //
  // begin() drives every relay line to its inactive electrical level and only
  // then switches the pin to an output, so the module sees at most the brief
  // power-on high-impedance period rather than a commanded pulse.
  // ---------------------------------------------------------------------
  g_pump.begin(millis());

  Serial.begin(SERIAL_BAUD);
  const uint32_t serialWaitStart = millis();
  while (!Serial &&
         static_cast<uint32_t>(millis() - serialWaitStart) < SERIAL_WAIT_MS) {
    // Bounded: the controller must run headless with no USB host attached.
  }

  Serial.println();
  Serial.println(F("=== fire-pump-controller ==="));
  Serial.print(F("Firmware: "));
  Serial.println(FIRMWARE_VERSION);
  Serial.print(F("Relay polarity: active-"));
  Serial.println(RELAY_ACTIVE_LOW ? F("LOW") : F("HIGH"));
  Serial.println(F("Initial state: UNKNOWN (engine status unproven after reset)"));
  Serial.print(F("Reset reason: "));
  Serial.println(toString(resetReason));
  Serial.print(F("Boots since power-on: "));
  Serial.print(bootCount());
  Serial.print(F("  faults: "));
  Serial.println(faultCountSincePowerOn());

  g_matrix.begin();
  g_console.begin();
  tankLevel().begin(millis());
  battery().begin(millis());

  g_net.begin(millis());
  g_http.begin();

  setupWatchdog();

  g_lastHeartbeatAt = millis();
}

void loop() {
  const uint32_t now = millis();

  // 1 + 2. Hard relay safety limits, then the pump state machine. Always
  //        first, before any network work, on every single iteration.
  g_pump.tick(now);

  // 3 + 4. Service an in-flight HTTP client incrementally, then accept a new
  //        one. Both are bounded and never wait for a byte.
  g_http.service(now, g_pump, g_net);

  // 4b. Bench console over USB, when compiled in. Bounded and non-blocking.
  g_console.tick(now, g_pump, g_net);

  // 4c. A requested network scan. Done here rather than in the console
  //     because WiFi.scanNetworks() blocks for several seconds -- close to
  //     the watchdog period -- so the watchdog is fed either side of it.
  //     The console only accepts the request while the relays are idle.
  if (g_console.wantsScan()) {
    g_console.clearScanRequest();

    // Take the radio away from the reconnect loop first. WiFiS3 multiplexes
    // one AT link to the ESP32-S3, so scanning while an association is in
    // flight just returns an empty list.
    g_net.suspend();
    feedWatchdog();
    WiFi.disconnect();
    feedWatchdog();

    const int found = WiFi.scanNetworks();
    feedWatchdog();
    Serial.print(F("networks found: "));
    Serial.println(found);
    for (int i = 0; i < found; ++i) {
      Serial.print(F("  ["));
      Serial.print(i);
      Serial.print(F("] rssi "));
      Serial.print(WiFi.RSSI(i));
      Serial.print(F(" dBm  enc "));
      Serial.print(WiFi.encryptionType(i));
      Serial.print(F("  ssid \""));
      Serial.print(WiFi.SSID(i));
      Serial.println(F("\""));
    }
    Serial.println(F("(only 2.4 GHz networks can appear; this radio has no 5 GHz)"));
    feedWatchdog();

    g_net.resume(millis());
  }

  // 5. Wi-Fi maintenance. A new association is only ever *initiated* while
  //    the controller is quiescent, so reconnection can never stretch out a
  //    starter, choke or kill timing window. A running lamp test counts as
  //    busy, so the radio is left alone while relays are being pulsed.
  g_net.tick(now, g_pump.isQuiescent() && !g_console.lampTestActive());

  // Diagnostic only, and deliberately after every safety-critical step: one
  // analogRead per sample interval, gating nothing.
  tankLevel().tick(now);

  // Diagnostic only, and after every safety-critical step. `relaysActive`
  // matters: a resting voltage measured while a coil is drawing is not a
  // resting voltage.
  battery().tick(now, g_pump.state(), !g_pump.isQuiescent());

  // Cosmetic status display. Last, so it can never delay anything that
  // matters, and rate-limited internally to a frame every ~100 ms.
  g_matrix.tick(now, g_pump.state(), g_pump.fault());

  if (static_cast<uint32_t>(now - g_lastHeartbeatAt) >= HEARTBEAT_INTERVAL_MS) {
    g_lastHeartbeatAt = now;
    printHeartbeat(now);
  }

  // 6. Feed the watchdog only after a complete, successful iteration.
  feedWatchdog();
}
