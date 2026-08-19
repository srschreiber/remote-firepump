// tests/test_valve_water.cpp — intake valve, water interlock, and the
// fail-safe run-enable wiring.
//
// These are the invariants that protect the pump's mechanical seal and the
// engine. Each is asserted from every angle it can be attacked from: through
// the API, through the maintenance layer, through the relay layer directly,
// under stalled loops, and across randomised command sequences.
//
// The two the whole design turns on:
//
//   1. The engine can never be cranked before the pump is confirmed primed.
//   2. The intake can never be shut while the engine may be running.

#include "test_support.h"

namespace {

const PumpState kAllStates[] = {
    PumpState::UNKNOWN,   PumpState::IDLE,          PumpState::PRIMING,
    PumpState::CHOKING,   PumpState::CRANKING,      PumpState::UNCHOKING,
    PumpState::RUNNING_ASSUMED, PumpState::STOPPING, PumpState::VALVE_CLOSING,
    PumpState::RETRY_WAIT, PumpState::FAULT,
};

// Was the starter ever driven active while the valve pin was inactive?
bool starterEverCrankedWithIntakeShut() {
  bool starterOn = false;
  bool valveOpen = false;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    if (e.pin == PIN_RELAY_STARTER) {
      starterOn = (e.value == activeLevel());
    } else if (e.pin == PIN_RELAY_VALVE) {
      valveOpen = (e.value == activeLevel());
    } else {
      continue;
    }
    if (starterOn && !valveOpen) {
      return true;
    }
  }
  return false;
}

// Earliest moment the starter went active, or UINT32_MAX.
uint32_t firstCrankAt() {
  for (const fake::Event& e : fake::events) {
    if (e.kind == fake::EventKind::DIGITAL_WRITE &&
        e.pin == PIN_RELAY_STARTER && e.value == activeLevel()) {
      return e.at;
    }
  }
  return UINT32_MAX;
}

uint32_t firstValveOpenAt() {
  for (const fake::Event& e : fake::events) {
    if (e.kind == fake::EventKind::DIGITAL_WRITE &&
        e.pin == PIN_RELAY_VALVE && e.value == activeLevel()) {
      return e.at;
    }
  }
  return UINT32_MAX;
}

}  // namespace

// ===========================================================================
// Fail-safe run-enable wiring (K3 on NC)
// ===========================================================================

TEST(boot_leaves_the_kill_wire_grounded_so_the_engine_cannot_run) {
  PumpController p;
  bootAt(p, 0);

  CHECK_MSG(p.killActive(),
            "boot did not assert kill; a magneto engine would be free to run");
  // Physically: K3 de-energised, NC contact closed, kill wire grounded.
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], inactiveLevel());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], killPinLevelFor(true));
}

TEST(power_loss_equivalent_grounds_kill_and_shuts_the_intake) {
  // A reset drives every pin to its inactive level. For K3 that closes the NC
  // contact and stops the engine; for K4 the NC valve shuts. The ORDER cannot
  // be controlled in hardware, which is exactly why the hardwired water
  // interlock is mandatory -- but the firmware's resting state must at least
  // be the safe one.
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);
  CHECK(!p.killActive());              // engine permitted to run
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], activeLevel());   // K3 energised

  // Simulate the reset.
  bootAt(p, 999999);

  CHECK_MSG(p.killActive(), "post-reset state does not inhibit the engine");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], inactiveLevel());
  CHECK(!p.valveActive());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_VALVE], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "UNKNOWN");
}

TEST(idle_keeps_the_engine_inhibited) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);
  CHECK_MSG(p.killActive(), "IDLE released the kill; engine could be started");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], killPinLevelFor(true));
}

TEST(the_engine_is_only_permitted_to_run_while_actually_starting_or_running) {
  for (PumpState s : kAllStates) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));

    const bool permitted = !p.killActive();
    const bool shouldBePermitted = (s == PumpState::CRANKING ||
                                    s == PumpState::UNCHOKING ||
                                    s == PumpState::RUNNING_ASSUMED);
    CHECK_MSG(permitted == shouldBePermitted,
              std::string("kill state wrong in ") + toString(s));
  }
}

TEST(stop_leaves_the_kill_asserted_permanently) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  p.handleCommand(CommandType::STOP, "s-1", fake::nowMs);
  CHECK(p.killActive());

  // Through the kill hold, the valve close, and long afterwards, the kill
  // must never be released. Releasing it would let the engine be restarted
  // by a pull cord or a stray starter press with nothing inhibiting it.
  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS + 60000, 100);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_MSG(p.killActive(), "the kill was released after the stop completed");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], killPinLevelFor(true));
}

// ===========================================================================
// INVARIANT 1: the engine can never crank before the pump is primed
// ===========================================================================

TEST(the_full_start_sequence_primes_before_it_cranks) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const CommandResult r = p.handleCommand(CommandType::START, "s-1", fake::nowMs);
  CHECK(r.accepted);

  if (INTAKE_VALVE_ENABLED) {
    CHECK_STREQ(toString(p.state()), "PRIMING");
    CHECK_MSG(p.valveActive(), "START did not open the intake");
    CHECK_MSG(!p.starterActive(), "START cranked immediately");
    CHECK_MSG(p.killActive(), "START released the kill before cranking");

    // Still priming one millisecond before the dwell expires.
    advanceBy(p, VALVE_PRIME_MS - 1);
    CHECK_STREQ(toString(p.state()), "PRIMING");
    CHECK(!p.starterActive());

    advanceBy(p, 1);
    CHECK_STREQ(toString(p.state()), "CHOKING");
  }

  advanceBy(p, CHOKE_PREP_MS);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  CHECK(p.starterActive());
  CHECK_MSG(!p.killActive(), "cranking with the kill still asserted");

  if (INTAKE_VALVE_ENABLED) {
    // Measured from the pin log: the crank began at least a full prime dwell
    // after the intake opened.
    const uint32_t opened = firstValveOpenAt();
    const uint32_t cranked = firstCrankAt();
    CHECK(opened != UINT32_MAX && cranked != UINT32_MAX);
    CHECK_MSG(static_cast<uint32_t>(cranked - opened) >= VALVE_PRIME_MS,
              "cranked before the prime dwell had elapsed");
  }
  CHECK_MSG(!starterEverCrankedWithIntakeShut(),
            "the starter was engaged with the intake shut");
}

TEST(the_relay_layer_refuses_to_crank_an_unprimed_pump) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // Release the kill by hand so the only thing standing in the way is the
  // prime interlock, then ask for the starter directly.
  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setKill(p, false);
  PumpTestAccess::setStarter(p, true);

  CHECK_MSG(!p.starterActive(), "cranked with the intake shut");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "VALVE_CLOSED_WHILE_RUNNING");
}

TEST(an_open_intake_alone_is_not_enough_the_dwell_must_elapse) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setValve(p, true);      // intake open...
  PumpTestAccess::setKill(p, false);
  PumpTestAccess::setStarter(p, true);    // ...but only just

  CHECK_MSG(!p.starterActive(),
            "cranked on a freshly opened intake without the prime dwell");
  CHECK_STREQ(toString(p.fault()), "VALVE_CLOSED_WHILE_RUNNING");
}

TEST(the_starter_engages_once_the_dwell_has_actually_elapsed) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setValve(p, true);
  advanceBy(p, VALVE_PRIME_MS);
  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setKill(p, false);
  PumpTestAccess::setStarter(p, true);

  CHECK_MSG(p.starterActive(), "refused to crank a properly primed pump");
  CHECK(toString(p.fault()) == nullptr);
}

TEST(no_reachable_state_ever_cranks_with_the_intake_shut) {
  for (PumpState s : kAllStates) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    advanceBy(p, 90000, 50);
    CHECK_MSG(!starterEverCrankedWithIntakeShut(),
              std::string("cranked with the intake shut, reaching ") + toString(s));
    checkStarterNeverCrankedWithKillAsserted(toString(s));
  }
}

// ===========================================================================
// INVARIANT 2: the intake can never be shut while the engine may be running
// ===========================================================================

TEST(the_relay_layer_refuses_to_shut_the_intake_while_running) {
  if (!INTAKE_VALVE_ENABLED) return;

  const PumpState running[] = {PumpState::CRANKING, PumpState::UNCHOKING,
                               PumpState::RUNNING_ASSUMED, PumpState::STOPPING};
  for (PumpState s : running) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    CHECK_MSG(p.valveActive(),
              std::string("intake was not open in ") + toString(s));

    PumpTestAccess::setNow(p, fake::nowMs);
    PumpTestAccess::setValve(p, false);   // direct attempt to shut it

    CHECK_MSG(p.valveActive(),
              std::string("intake was shut while running, in ") + toString(s));
    CHECK_EQ(fake::pinLevel[PIN_RELAY_VALVE], activeLevel());
  }
}

TEST(the_stop_sequence_shuts_the_intake_only_after_the_engine_has_stopped) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);
  CHECK(p.valveActive());

  p.handleCommand(CommandType::STOP, "s-1", fake::nowMs);
  CHECK_STREQ(toString(p.state()), "STOPPING");
  CHECK_MSG(p.killActive(), "STOP did not ground the kill wire");
  CHECK_MSG(p.valveActive(), "STOP shut the intake immediately");

  // Right through the kill hold the intake stays open.
  advanceBy(p, KILL_HOLD_MS - 1);
  CHECK_MSG(p.valveActive(), "intake shut during the kill hold");

  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "VALVE_CLOSING");
  CHECK_MSG(p.valveActive(), "intake shut the instant the kill hold ended");

  // And only after the engine has been given time to come to rest.
  advanceBy(p, VALVE_CLOSE_DELAY_MS - 1);
  CHECK_MSG(p.valveActive(), "intake shut before VALVE_CLOSE_DELAY_MS elapsed");

  advanceBy(p, 1);
  CHECK_MSG(!p.valveActive(), "intake never shut");
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_VALVE], inactiveLevel());

  // Total: the intake stayed open for at least kill hold + close delay after
  // the stop was commanded.
  CHECK(p.killActive());
}

TEST(maintenance_cannot_shut_the_intake_while_the_engine_may_be_running) {
  if (!INTAKE_VALVE_ENABLED || !MAINTENANCE_API_ENABLED) return;

  const PumpState running[] = {PumpState::CRANKING, PumpState::UNCHOKING,
                               PumpState::RUNNING_ASSUMED, PumpState::STOPPING};
  for (PumpState s : running) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));

    const CommandResult r =
        p.handleCommand(CommandType::MAINT_VALVE_OFF, "m-1", fake::nowMs);
    CHECK_MSG(!r.accepted,
              std::string("maintenance shut the intake in ") + toString(s));
    CHECK_MSG(p.valveActive(), "intake was shut anyway");
  }
}

TEST(a_fault_never_shuts_the_intake_on_a_running_engine) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);
  CHECK(p.valveActive());

  // Provoke the starter-overrun backstop by stalling the loop.
  jumpBy(p, MAX_CRANK_MS + 1);
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_MSG(p.valveActive(),
            "faulting shut the intake while the engine could be running");
  CHECK_MSG(p.killActive(), "faulting did not ground the kill wire");
}

// ===========================================================================
// Water interlock
// ===========================================================================

TEST(start_is_refused_without_water) {
  if (!WATER_INTERLOCK_REQUIRED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);
  setWaterAvailable(p, false);
  CHECK(!p.waterOk());

  const CommandResult r = p.handleCommand(CommandType::START, "s-1", fake::nowMs);
  CHECK_MSG(!r.accepted, "START was accepted with no water available");
  CHECK_EQ(r.httpStatus, 409);
  CHECK(!p.valveActive());
  CHECK(!p.starterActive());
}

TEST(a_broken_sensor_wire_reads_as_no_water) {
  if (!WATER_INTERLOCK_REQUIRED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // INPUT_PULLUP with nothing connected floats HIGH, which is "no water".
  fake::pinLevel[PIN_WATER_OK] = HIGH;
  advanceBy(p, WATER_DEBOUNCE_MS + 50, 25);

  CHECK_MSG(!p.waterOk(), "an open-circuit sensor was read as water present");
  const CommandResult r = p.handleCommand(CommandType::START, "s-1", fake::nowMs);
  CHECK(!r.accepted);
}

TEST(the_water_reading_is_debounced) {
  if (!WATER_INTERLOCK_REQUIRED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);
  CHECK(p.waterOk());

  // A brief dip must not be believed -- pressure switches chatter.
  setWaterRaw(false);
  advanceBy(p, WATER_DEBOUNCE_MS - 50, 10);
  CHECK_MSG(p.waterOk(), "a momentary dip was believed immediately");

  setWaterRaw(true);
  advanceBy(p, WATER_DEBOUNCE_MS + 50, 10);
  CHECK(p.waterOk());

  // A sustained loss is believed.
  setWaterRaw(false);
  advanceBy(p, WATER_DEBOUNCE_MS + 50, 10);
  CHECK_MSG(!p.waterOk(), "a sustained loss was never believed");
}

TEST(losing_water_while_running_stops_the_engine) {
  if (!WATER_INTERLOCK_REQUIRED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);
  CHECK(!p.killActive());   // engine permitted to run

  // Wait out the startup grace, then lose water.
  advanceBy(p, WATER_STARTUP_GRACE_MS + 100, 100);
  setWaterAvailable(p, false);

  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "WATER_LOST");
  CHECK_MSG(p.killActive(), "water was lost but the engine was not stopped");
  CHECK_MSG(!p.starterActive(), "starter left engaged after water loss");
  CHECK_MSG(p.valveActive(),
            "the intake was shut on a possibly-running engine after water loss");
}

TEST(a_dip_during_the_startup_grace_does_not_stop_the_engine) {
  if (!WATER_INTERLOCK_REQUIRED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  // Straight after a crank the pump has not built pressure yet.
  setWaterAvailable(p, false);
  CHECK_MSG(toString(p.fault()) == nullptr,
            "a dip inside the startup grace shut the engine down");
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");

  // Once the grace expires it is acted on.
  advanceBy(p, WATER_STARTUP_GRACE_MS + 100, 100);
  CHECK_STREQ(toString(p.fault()), "WATER_LOST");
}

TEST(water_is_reported_in_status) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  std::string body = doRequest(
      p, makeRequest("GET", "/v1/status", kTestSecret, nullptr), status,
      fake::nowMs);
  CHECK_EQ(status, 200);
  CHECK_CONTAINS(body, "\"water_ok\":true");
  CHECK_CONTAINS(body, INTAKE_VALVE_ENABLED ? "\"intake_valve_enabled\":true"
                                            : "\"intake_valve_enabled\":false");
  CHECK_CONTAINS(body, "\"valve\":false");
  CHECK_NOT_CONTAINS(body, "\"spare\"");

  if (WATER_INTERLOCK_REQUIRED) {
    setWaterAvailable(p, false);
    body = doRequest(p, makeRequest("GET", "/v1/status", kTestSecret, nullptr),
                     status, fake::nowMs);
    CHECK_CONTAINS(body, "\"water_ok\":false");
  }
}

TEST(status_reports_the_valve_timings) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  const std::string body = doRequest(
      p, makeRequest("GET", "/v1/status", kTestSecret, nullptr), status,
      fake::nowMs);
  CHECK_CONTAINS(body, "\"valve_prime_ms\":5000");
  CHECK_CONTAINS(body, "\"valve_close_delay_ms\":3000");
}

// ===========================================================================
// Ordering, proven from the recorded pin log
// ===========================================================================

TEST(a_full_start_stop_cycle_has_the_correct_relay_ordering) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const size_t mark = fake::events.size();
  p.handleCommand(CommandType::START, "cycle-1", fake::nowMs);
  advanceBy(p, VALVE_PRIME_MS + CHOKE_PREP_MS + CRANK_DURATION_MS +
                   UNCHOKE_DELAY_MS + 100);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");

  p.handleCommand(CommandType::STOP, "cycle-2", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS + 100);
  CHECK_STREQ(toString(p.state()), "IDLE");

  // Extract the ordered sequence of interesting edges.
  size_t valveOpen = SIZE_MAX, killReleased = SIZE_MAX, crankOn = SIZE_MAX;
  size_t crankOff = SIZE_MAX, killAsserted = SIZE_MAX, valveShut = SIZE_MAX;
  for (size_t i = mark; i < fake::events.size(); ++i) {
    const fake::Event& e = fake::events[i];
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    const bool on = (e.value == activeLevel());
    if (e.pin == PIN_RELAY_VALVE && on && valveOpen == SIZE_MAX) valveOpen = i;
    if (e.pin == PIN_RELAY_VALVE && !on && crankOff != SIZE_MAX &&
        valveShut == SIZE_MAX) valveShut = i;
    // K3 energised == kill released.
    if (e.pin == PIN_RELAY_KILL && on && killReleased == SIZE_MAX) killReleased = i;
    if (e.pin == PIN_RELAY_KILL && !on && killReleased != SIZE_MAX &&
        killAsserted == SIZE_MAX) killAsserted = i;
    if (e.pin == PIN_RELAY_STARTER && on && crankOn == SIZE_MAX) crankOn = i;
    if (e.pin == PIN_RELAY_STARTER && !on && crankOn != SIZE_MAX &&
        crankOff == SIZE_MAX) crankOff = i;
  }

  if (INTAKE_VALVE_ENABLED) {
    CHECK_MSG(valveOpen != SIZE_MAX, "the intake never opened");
    CHECK_MSG(valveOpen < killReleased,
              "the kill was released before the intake opened");
    CHECK_MSG(valveShut != SIZE_MAX, "the intake never shut");
    CHECK_MSG(killAsserted < valveShut,
              "the intake was shut before the kill was asserted");
  }
  CHECK_MSG(killReleased != SIZE_MAX, "the kill was never released");
  CHECK_MSG(killReleased < crankOn, "cranked before the kill was released");
  CHECK_MSG(crankOn < crankOff, "the starter never released");
  CHECK_MSG(crankOff < killAsserted, "the kill was asserted before the crank ended");

  checkStarterNeverCrankedWithKillAsserted("full cycle");
  CHECK(!starterEverCrankedWithIntakeShut());
}
