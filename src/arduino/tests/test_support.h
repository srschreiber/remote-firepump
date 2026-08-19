// tests/test_support.h — shared helpers for the host test suite.

#pragma once

#include "shim/Arduino.h"
#include "test_framework.h"

#include "../fire_pump_controller/api_handler.h"
#include "../fire_pump_controller/config.h"
#include "../fire_pump_controller/http_protocol.h"
#include "../fire_pump_controller/pump_controller.h"

#include <string>

// Backdoor into PumpController's private relay layer. Only exists because
// PUMP_CONTROLLER_TEST_ACCESS is defined for this build; firmware never sees
// it. Lets tests provoke the defensive interlocks directly instead of hoping
// the state machine happens to route through them.
struct PumpTestAccess {
  static void setNow(PumpController& p, uint32_t n) { p.now_ = n; }

  static void setStarter(PumpController& p, bool a) { p.setStarterRelay(a); }
  static void setChoke(PumpController& p, bool a) { p.setChokeRelay(a); }
  static void setKill(PumpController& p, bool a) { p.setKillRelay(a); }
  static void setValve(PumpController& p, bool a) { p.setValveRelay(a); }

  // Simulates a corrupted/blown-through internal state that enforceSafety()
  // must catch, without needing a real hardware fault.
  static void corruptValveFlag(PumpController& p, bool a) { p.valveActive_ = a; }
  static void corruptStarterFlag(PumpController& p, bool a) { p.starterActive_ = a; }
  static void corruptKillFlag(PumpController& p, bool a) { p.killActive_ = a; }
  static void setStarterOnAt(PumpController& p, uint32_t t) { p.starterOnAt_ = t; }
  static void setChokeOnAt(PumpController& p, uint32_t t) { p.chokeOnAt_ = t; }

  static uint8_t recordSlotsUsed(const PumpController& p) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < IDEMPOTENCY_SLOTS; ++i) {
      if (p.records_[i].used) ++n;
    }
    return n;
  }
};

// Electrical level that corresponds to a commanded-active / -inactive relay,
// derived from the single RELAY_ACTIVE_LOW constant under test.
inline uint8_t activeLevel() { return RELAY_ACTIVE_LOW ? LOW : HIGH; }
inline uint8_t inactiveLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW; }

// K3 is wired to its NC contact, so the relay must be ENERGISED to permit the
// engine to run. "Kill asserted" therefore means the relay is de-energised.
inline bool killRelayEnergised(bool killAsserted) {
  return KILL_RELAY_FAIL_SAFE_NC ? !killAsserted : killAsserted;
}
inline uint8_t killPinLevelFor(bool killAsserted) {
  return killRelayEnergised(killAsserted) ? activeLevel() : inactiveLevel();
}

// --- water interlock -------------------------------------------------------

// Sets the raw level on the water-available input. The reading still has to
// survive WATER_DEBOUNCE_MS before the controller believes it.
void setWaterRaw(bool available);

// Sets the input and ticks long enough for the debounce to settle.
void setWaterAvailable(PumpController& p, bool available);

// --- clock driving ---------------------------------------------------------

// Sets the virtual clock and ticks the controller once.
void tickAt(PumpController& p, uint32_t t);

// Steps the clock forward `dt` ms in `step` increments, ticking each time.
// This mirrors the real main loop, which ticks far more often than any timer
// boundary.
void advanceBy(PumpController& p, uint32_t dt, uint32_t step = 1);

// A single, large clock jump with one tick: simulates a stalled main loop.
void jumpBy(PumpController& p, uint32_t dt);

// --- fixtures --------------------------------------------------------------

// Fresh controller at t = startMs, in UNKNOWN with all relays inactive.
void bootAt(PumpController& p, uint32_t startMs);

// Drives a freshly booted controller into `target`. Returns false if the
// target state could not be reached (which is itself a test failure).
bool driveToState(PumpController& p, PumpState target);

// --- API helpers -----------------------------------------------------------

extern const char* const kTestSecret;

// Feeds a complete raw HTTP request through the parser, then plans a
// response. Returns the response body as a std::string; `outStatus` receives
// the HTTP status.
std::string doRequest(PumpController& pump, const std::string& raw,
                      uint16_t& outStatus, uint32_t nowMs,
                      const char* secret = kTestSecret);

// Convenience: builds a well-formed request with optional headers.
std::string makeRequest(const char* method, const char* path,
                        const char* secret /* nullptr => omit header */,
                        const char* requestId /* nullptr => omit header */);

// --- relay assertions ------------------------------------------------------

// Asserts the controller's logical view and the physical pin levels agree,
// including the K3 fail-safe inversion.
void checkRelayPins(const PumpController& p, const char* where);

// Asserts the starter pin was never driven active while the kill relay was
// de-energised (kill asserted) -- checked over the whole recorded event log.
void checkStarterNeverCrankedWithKillAsserted(const char* where);
