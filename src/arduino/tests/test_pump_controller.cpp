// tests/test_pump_controller.cpp — state machine, relay layer and interlocks.

#include "test_support.h"

namespace {

// Total time PIN_RELAY_STARTER spent at its active level, computed from the
// recorded pin-event log rather than from the controller's own bookkeeping.
uint32_t starterActiveTotalMs() {
  uint32_t total = 0;
  bool on = false;
  uint32_t onAt = 0;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) {
      on = true;
      onAt = e.at;
    } else if (!nowOn && on) {
      on = false;
      total += static_cast<uint32_t>(e.at - onAt);
    }
  }
  if (on) {
    total += static_cast<uint32_t>(fake::nowMs - onAt);
  }
  return total;
}

// Longest single uninterrupted active period of the starter pin.
uint32_t starterLongestActiveMs() {
  uint32_t longest = 0;
  bool on = false;
  uint32_t onAt = 0;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) {
      on = true;
      onAt = e.at;
    } else if (!nowOn && on) {
      on = false;
      const uint32_t d = static_cast<uint32_t>(e.at - onAt);
      if (d > longest) longest = d;
    }
  }
  if (on) {
    const uint32_t d = static_cast<uint32_t>(fake::nowMs - onAt);
    if (d > longest) longest = d;
  }
  return longest;
}

// Longest uninterrupted starter-active period considering only events from
// `markIdx` onward, with the window starting at `markTime`.
//
// This matters because some fixtures deliberately stall the simulated main
// loop, which physically holds the relay for the length of the stall. That
// stall is an artefact of the fixture, not firmware behaviour; what the
// firmware guarantees is what it does once it is running again.
uint32_t starterLongestActiveMsFrom(size_t markIdx, uint32_t markTime,
                                    bool onAtMark) {
  uint32_t longest = 0;
  bool on = onAtMark;
  uint32_t onAt = markTime;
  for (size_t i = markIdx; i < fake::events.size(); ++i) {
    const fake::Event& e = fake::events[i];
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) {
      on = true;
      onAt = e.at;
    } else if (!nowOn && on) {
      on = false;
      const uint32_t d = static_cast<uint32_t>(e.at - onAt);
      if (d > longest) longest = d;
    }
  }
  if (on) {
    const uint32_t d = static_cast<uint32_t>(fake::nowMs - onAt);
    if (d > longest) longest = d;
  }
  return longest;
}

int starterActivationCount() {
  int n = 0;
  bool on = false;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) ++n;
    on = nowOn;
  }
  return n;
}

const PumpState kAllStates[] = {
    PumpState::UNKNOWN,   PumpState::IDLE,        PumpState::CHOKING,
    PumpState::CRANKING,  PumpState::UNCHOKING,   PumpState::RUNNING_ASSUMED,
    PumpState::STOPPING,  PumpState::RETRY_WAIT,  PumpState::FAULT,
};

}  // namespace

// ===========================================================================
// Boot behaviour
// ===========================================================================

TEST(boot_starts_in_unknown_with_all_relays_inactive) {
  PumpController p;
  bootAt(p, 0);

  CHECK_STREQ(toString(p.state()), "UNKNOWN");
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());
  CHECK(!p.killActive());
  CHECK(!p.spareActive());
  checkRelayPins(p, "boot");
  CHECK(!p.runningConfirmed());
  CHECK(toString(p.fault()) == nullptr);
}

TEST(boot_drives_every_pin_inactive_before_enabling_it_as_output) {
  PumpController p;
  bootAt(p, 12345);

  const uint8_t pins[] = {PIN_RELAY_STARTER, PIN_RELAY_CHOKE, PIN_RELAY_KILL,
                          PIN_RELAY_SPARE};
  for (uint8_t pin : pins) {
    // At least one digitalWrite must precede pinMode(OUTPUT) so the output
    // data register already holds the safe level when the pin starts driving.
    CHECK_MSG(fake::writesBeforeMode(pin) >= 1,
              "pin was made an output before being pre-driven");
    CHECK_EQ(fake::pinModeOf[pin], OUTPUT);
    CHECK_EQ(fake::pinLevel[pin], inactiveLevel());
  }

  // And no pin was ever driven to the active level during startup.
  for (uint8_t pin : pins) {
    CHECK_MSG(fake::firstWriteIndex(pin, activeLevel()) == SIZE_MAX,
              "a relay was pulsed active during setup()");
  }
}

TEST(boot_after_reboot_never_reports_running_or_idle) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");

  // Simulate a power cycle / watchdog reset.
  bootAt(p, 999999);
  CHECK_STREQ(toString(p.state()), "UNKNOWN");
  CHECK_STREQ(toString(p.engineStatus()), "UNKNOWN");
  CHECK(!p.runningConfirmed());
  CHECK_EQ(p.cooldownRemainingMs(fake::nowMs), 0u);
}

// ===========================================================================
// START gating
// ===========================================================================

TEST(start_is_rejected_from_unknown) {
  PumpController p;
  bootAt(p, 0);
  const CommandResult r = p.handleCommand(CommandType::START, "s1", fake::nowMs);
  CHECK(!r.accepted);
  CHECK_EQ(r.httpStatus, 409);
  CHECK_STREQ(toString(p.state()), "UNKNOWN");
  CHECK_EQ(starterActivationCount(), 0);
}

TEST(start_is_rejected_from_every_state_except_idle) {
  for (PumpState s : kAllStates) {
    PumpController p;
    bootAt(p, 0);
    CHECK_MSG(driveToState(p, s), "fixture could not reach target state");

    const PumpState before = p.state();
    const CommandResult r = p.handleCommand(CommandType::START, "probe", fake::nowMs);

    if (s == PumpState::IDLE) {
      // IDLE reached straight from UNKNOWN has no outstanding cooldown.
      CHECK_MSG(r.accepted, "START must be accepted from a cooled-down IDLE");
      CHECK_EQ(r.httpStatus, 202);
    } else {
      CHECK_MSG(!r.accepted, "START accepted from a non-IDLE state");
      CHECK_EQ(r.httpStatus, 409);
      CHECK_MSG(p.state() == before, "a rejected START changed the state");
    }
  }
}

TEST(start_is_rejected_from_idle_while_recrank_cooldown_is_outstanding) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  // STOP returns to IDLE after KILL_HOLD_MS, which is shorter than the
  // recrank gap, so IDLE is reached with cooldown still running.
  p.handleCommand(CommandType::STOP, "stop-1", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK(p.cooldownRemainingMs(fake::nowMs) > 0);

  const CommandResult r = p.handleCommand(CommandType::START, "s2", fake::nowMs);
  CHECK(!r.accepted);
  CHECK_EQ(r.httpStatus, 409);
  CHECK(r.cooldownRemainingMs > 0);
  CHECK_EQ(starterActivationCount(), 1);

  // Once the gap expires the same command is permitted.
  advanceBy(p, p.cooldownRemainingMs(fake::nowMs), 100);
  CHECK_EQ(p.cooldownRemainingMs(fake::nowMs), 0u);
  const CommandResult r2 = p.handleCommand(CommandType::START, "s3", fake::nowMs);
  CHECK(r2.accepted);
}

// ===========================================================================
// Start sequence timing
// ===========================================================================

TEST(start_sequence_follows_choke_crank_release_unchoke_with_exact_timings) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const CommandResult r = p.handleCommand(CommandType::START, "run-1", fake::nowMs);
  CHECK(r.accepted);
  CHECK_EQ(r.httpStatus, 202);
  CHECK_STREQ(toString(r.state), "CHOKING");

  // Step 3/4: choke engaged, starter and kill open.
  CHECK(p.chokeActive());
  CHECK(!p.starterActive());
  CHECK(!p.killActive());
  checkRelayPins(p, "CHOKING");

  // Still choking one millisecond before the prep time elapses.
  advanceBy(p, CHOKE_PREP_MS - 1);
  CHECK_STREQ(toString(p.state()), "CHOKING");
  CHECK(!p.starterActive());

  // Step 5: starter engages exactly at CHOKE_PREP_MS.
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  CHECK(p.starterActive());
  CHECK(p.chokeActive());
  CHECK(!p.killActive());
  checkRelayPins(p, "CRANKING");

  advanceBy(p, CRANK_DURATION_MS - 1);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  CHECK(p.starterActive());

  // Step 6/7: starter releases exactly at CRANK_DURATION_MS.
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "UNCHOKING");
  CHECK(!p.starterActive());
  CHECK(p.chokeActive());
  checkRelayPins(p, "UNCHOKING");

  advanceBy(p, UNCHOKE_DELAY_MS - 1);
  CHECK_STREQ(toString(p.state()), "UNCHOKING");
  CHECK(p.chokeActive());

  // Step 8/9: choke releases, RUNNING_ASSUMED.
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  CHECK(!p.chokeActive());
  CHECK(!p.starterActive());
  CHECK(!p.killActive());
  checkRelayPins(p, "RUNNING_ASSUMED");

  // Measured from the pin event log, not from internal bookkeeping.
  CHECK_EQ(starterActiveTotalMs(), CRANK_DURATION_MS);
  CHECK_EQ(starterActivationCount(), 1);
  checkSpareNeverActive("start sequence");

  // No sensor exists, so the engine is never reported as confirmed running.
  CHECK(!p.runningConfirmed());
  CHECK_STREQ(toString(p.engineStatus()), "RUNNING_ASSUMED");
}

TEST(no_automatic_retry_after_reaching_running_assumed) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  // Sit for well over a minute. Nothing may re-crank on its own.
  advanceBy(p, 120000, 250);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  CHECK_EQ(starterActivationCount(), 1);
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());
  CHECK(!p.killActive());
}

TEST(choke_never_remains_active_after_a_completed_start) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);
  advanceBy(p, 60000, 500);
  CHECK(!p.chokeActive());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_CHOKE], inactiveLevel());
}

// ===========================================================================
// STOP
// ===========================================================================

TEST(stop_is_accepted_from_every_state) {
  for (PumpState s : kAllStates) {
    PumpController p;
    bootAt(p, 0);
    CHECK_MSG(driveToState(p, s), "fixture could not reach target state");

    const CommandResult r = p.handleCommand(CommandType::STOP, "stop-any", fake::nowMs);
    CHECK_MSG(r.accepted, "STOP was rejected");
    CHECK_EQ(r.httpStatus, 202);
    CHECK_STREQ(toString(p.state()), "STOPPING");

    // Immediately: starter off, choke off, kill on.
    CHECK(!p.starterActive());
    CHECK(!p.chokeActive());
    CHECK(p.killActive());
    checkRelayPins(p, "after STOP");

    // Kill is held, then released, landing in IDLE.
    advanceBy(p, KILL_HOLD_MS - 1);
    CHECK_STREQ(toString(p.state()), "STOPPING");
    CHECK(p.killActive());

    advanceBy(p, 1);
    CHECK_STREQ(toString(p.state()), "IDLE");
    CHECK(!p.killActive());
    checkRelayPins(p, "after kill hold");
    checkSpareNeverActive("stop from state");
  }
}

TEST(stop_during_cranking_releases_starter_before_grounding_kill) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);
  CHECK(p.starterActive());

  const size_t before = fake::events.size();
  p.handleCommand(CommandType::STOP, "stop-crank", fake::nowMs);

  // Find, among the events produced by this command, the starter-inactive
  // write and the kill-active write, and assert the ordering.
  size_t starterOffIdx = SIZE_MAX;
  size_t killOnIdx = SIZE_MAX;
  for (size_t i = before; i < fake::events.size(); ++i) {
    const fake::Event& e = fake::events[i];
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    if (e.pin == PIN_RELAY_STARTER && e.value == inactiveLevel() &&
        starterOffIdx == SIZE_MAX) {
      starterOffIdx = i;
    }
    if (e.pin == PIN_RELAY_KILL && e.value == activeLevel() &&
        killOnIdx == SIZE_MAX) {
      killOnIdx = i;
    }
  }

  CHECK(starterOffIdx != SIZE_MAX);
  CHECK(killOnIdx != SIZE_MAX);
  CHECK_MSG(starterOffIdx < killOnIdx,
            "kill was grounded before the starter was released");
  CHECK(!p.starterActive());
  CHECK(p.killActive());
}

TEST(stop_during_choking_cancels_choke_and_grounds_kill) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CHOKING);
  CHECK(p.chokeActive());

  p.handleCommand(CommandType::STOP, "stop-choke", fake::nowMs);
  CHECK(!p.chokeActive());
  CHECK(!p.starterActive());
  CHECK(p.killActive());
  CHECK_STREQ(toString(p.state()), "STOPPING");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_CHOKE], inactiveLevel());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], activeLevel());

  // The starter never engaged at all on this path.
  CHECK_EQ(starterActivationCount(), 0);
}

TEST(stop_from_running_assumed_holds_kill_for_the_configured_duration) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  const uint32_t t0 = fake::nowMs;
  p.handleCommand(CommandType::STOP, "stop-run", fake::nowMs);
  CHECK(p.killActive());

  advanceBy(p, KILL_HOLD_MS);
  CHECK(!p.killActive());
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_EQ(static_cast<uint32_t>(fake::nowMs - t0), KILL_HOLD_MS);
}

TEST(repeated_stop_extends_the_kill_hold_rather_than_shortening_it) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::STOPPING);

  advanceBy(p, KILL_HOLD_MS - 100);
  CHECK(p.killActive());

  // A second STOP restarts the hold window.
  p.handleCommand(CommandType::STOP, "stop-again", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS - 1);
  CHECK_MSG(p.killActive(), "second STOP shortened the kill hold");
  advanceBy(p, 1);
  CHECK(!p.killActive());
  CHECK_STREQ(toString(p.state()), "IDLE");
}

TEST(stop_clears_a_fault_and_returns_to_idle) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::FAULT);
  CHECK(toString(p.fault()) != nullptr);

  const CommandResult r = p.handleCommand(CommandType::STOP, "stop-fault", fake::nowMs);
  CHECK(r.accepted);
  CHECK(toString(p.fault()) == nullptr);
  advanceBy(p, KILL_HOLD_MS);
  CHECK_STREQ(toString(p.state()), "IDLE");
}

// ===========================================================================
// start-failed
// ===========================================================================

TEST(start_failed_enters_retry_wait_and_falls_back_to_idle) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  const CommandResult r =
      p.handleCommand(CommandType::START_FAILED, "failed-1", fake::nowMs);
  CHECK(r.accepted);
  CHECK_EQ(r.httpStatus, 202);
  CHECK_STREQ(toString(p.state()), "RETRY_WAIT");

  // All three relays inactive.
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());
  CHECK(!p.killActive());
  checkRelayPins(p, "RETRY_WAIT");

  const uint32_t remaining = p.cooldownRemainingMs(fake::nowMs);
  CHECK(remaining > 0);

  // Stays in RETRY_WAIT for the whole gap, never auto-retrying.
  advanceBy(p, remaining - 1);
  CHECK_STREQ(toString(p.state()), "RETRY_WAIT");
  CHECK_EQ(starterActivationCount(), 1);

  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_EQ(starterActivationCount(), 1);

  // And still no automatic retry after landing in IDLE.
  advanceBy(p, 60000, 500);
  CHECK_EQ(starterActivationCount(), 1);
  CHECK_STREQ(toString(p.state()), "IDLE");
}

TEST(start_failed_enforces_minimum_interval_between_starter_attempts) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  const uint32_t releaseTime = fake::nowMs - UNCHOKE_DELAY_MS;
  p.handleCommand(CommandType::START_FAILED, "failed-2", fake::nowMs);

  // The gap is measured from the moment the starter was released.
  advanceBy(p, MIN_RECRANK_GAP_MS - UNCHOKE_DELAY_MS - 1);
  CHECK_STREQ(toString(p.state()), "RETRY_WAIT");
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_EQ(static_cast<uint32_t>(fake::nowMs - releaseTime), MIN_RECRANK_GAP_MS);
}

TEST(start_failed_is_rejected_during_an_active_sequence) {
  const PumpState busy[] = {PumpState::CHOKING, PumpState::CRANKING,
                            PumpState::UNCHOKING, PumpState::STOPPING,
                            PumpState::FAULT};
  for (PumpState s : busy) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    const CommandResult r =
        p.handleCommand(CommandType::START_FAILED, "f", fake::nowMs);
    CHECK_MSG(!r.accepted, "start-failed accepted mid-sequence");
    CHECK_EQ(r.httpStatus, 409);
  }
}

// ===========================================================================
// reset-idle
// ===========================================================================

TEST(reset_idle_moves_unknown_to_idle_and_deactivates_all_relays) {
  PumpController p;
  bootAt(p, 0);
  const CommandResult r =
      p.handleCommand(CommandType::RESET_IDLE, "reset-1", fake::nowMs);
  CHECK(r.accepted);
  CHECK_EQ(r.httpStatus, 202);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());
  CHECK(!p.killActive());
  checkRelayPins(p, "after reset-idle");
  CHECK_STREQ(toString(p.engineStatus()), "STOPPED_ASSUMED");
}

TEST(reset_idle_respects_an_outstanding_recrank_cooldown) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  // Reboot mid-cooldown is not simulated here; instead use FAULT, which keeps
  // the release timestamp, then reset.
  p.handleCommand(CommandType::START_FAILED, "f1", fake::nowMs);
  CHECK(p.cooldownRemainingMs(fake::nowMs) > 0);

  const CommandResult r =
      p.handleCommand(CommandType::RESET_IDLE, "reset-2", fake::nowMs);
  CHECK(r.accepted);
  CHECK_MSG(p.state() == PumpState::RETRY_WAIT,
            "reset-idle jumped straight to IDLE with a cooldown outstanding");
  CHECK(r.cooldownRemainingMs > 0);

  advanceBy(p, r.cooldownRemainingMs);
  CHECK_STREQ(toString(p.state()), "IDLE");
}

TEST(reset_idle_clears_a_fault) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::FAULT);
  CHECK(toString(p.fault()) != nullptr);

  // Wait out the fault kill hold so the relays are quiet first.
  advanceBy(p, KILL_HOLD_MS);
  CHECK(!p.killActive());

  const CommandResult r =
      p.handleCommand(CommandType::RESET_IDLE, "reset-3", fake::nowMs);
  CHECK(r.accepted);
  CHECK(toString(p.fault()) == nullptr);
  CHECK(p.state() == PumpState::RETRY_WAIT || p.state() == PumpState::IDLE);
}

TEST(reset_idle_is_rejected_during_an_active_sequence) {
  const PumpState busy[] = {PumpState::CHOKING, PumpState::CRANKING,
                            PumpState::UNCHOKING, PumpState::STOPPING,
                            PumpState::RUNNING_ASSUMED};
  for (PumpState s : busy) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    const CommandResult r =
        p.handleCommand(CommandType::RESET_IDLE, "r", fake::nowMs);
    CHECK_MSG(!r.accepted, "reset-idle accepted mid-sequence");
    CHECK_EQ(r.httpStatus, 409);
  }
}

// ===========================================================================
// Hard safety interlocks
// ===========================================================================

TEST(starter_is_forced_inactive_at_max_crank_even_if_the_loop_stalls) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);
  CHECK(p.starterActive());

  // The main loop stalls for longer than the absolute ceiling. One tick.
  jumpBy(p, MAX_CRANK_MS + 1);

  CHECK_MSG(!p.starterActive(), "starter survived past MAX_CRANK_MS");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "STARTER_OVERRUN");
  CHECK(starterLongestActiveMs() <= MAX_CRANK_MS + 1);
}

TEST(starter_engagement_never_exceeds_five_seconds_on_any_path) {
  // Every reachable state, entered and then run out for two minutes at a
  // range of realistic loop periods. MAX_CRANK_MS is separately asserted to
  // be <= 5000 at compile time.
  const uint32_t steps[] = {1, 7, 50, 250};
  for (PumpState s : kAllStates) {
    for (uint32_t step : steps) {
      PumpController p;
      bootAt(p, 0);
      CHECK(driveToState(p, s));

      // Measure only from here, so a fixture that deliberately stalls the
      // loop does not count against the firmware.
      const size_t mark = fake::events.size();
      const uint32_t markTime = fake::nowMs;
      const bool onAtMark = p.starterActive();

      advanceBy(p, 120000, step);

      const uint32_t longest =
          starterLongestActiveMsFrom(mark, markTime, onAtMark);
      CHECK_MSG(longest <= MAX_CRANK_MS, "starter exceeded MAX_CRANK_MS");
      CHECK_MSG(longest <= 5000, "starter exceeded the five second hard limit");
      CHECK_MSG(!p.starterActive(), "starter still engaged after two minutes");
    }
  }
}

TEST(a_stalled_main_loop_releases_the_starter_on_its_very_first_tick) {
  // If the loop stalls, the firmware cannot act until it runs again -- so the
  // guarantee it *can* make is that the first tick after the stall releases
  // the starter and faults, with no further grace period. Bounding the stall
  // itself is the hardware watchdog's job (see README, "Watchdog").
  const uint32_t stalls[] = {MAX_CRANK_MS + 1, 8000, 30000, 120000};
  for (uint32_t stall : stalls) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, PumpState::CRANKING));
    CHECK(p.starterActive());

    const size_t before = fake::events.size();
    jumpBy(p, stall);   // exactly one tick after the stall

    CHECK_MSG(!p.starterActive(), "starter survived the first post-stall tick");
    CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
    CHECK_STREQ(toString(p.state()), "FAULT");
    CHECK_STREQ(toString(p.fault()), "STARTER_OVERRUN");

    // The release really did happen on that tick, not later.
    bool releasedNow = false;
    for (size_t i = before; i < fake::events.size(); ++i) {
      const fake::Event& e = fake::events[i];
      if (e.kind == fake::EventKind::DIGITAL_WRITE &&
          e.pin == PIN_RELAY_STARTER && e.value == inactiveLevel()) {
        releasedNow = true;
      }
    }
    CHECK(releasedNow);
  }
}

TEST(choke_is_forced_inactive_at_max_choke) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CHOKING);
  CHECK(p.chokeActive());

  jumpBy(p, MAX_CHOKE_MS + 1);

  CHECK_MSG(!p.chokeActive(), "choke survived past MAX_CHOKE_MS");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_CHOKE], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "CHOKE_OVERRUN");
}

TEST(relay_layer_refuses_to_engage_the_starter_while_kill_is_grounded) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setKill(p, true);
  CHECK(p.killActive());

  PumpTestAccess::setStarter(p, true);

  CHECK_MSG(!p.starterActive(), "starter engaged while kill was grounded");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "STARTER_KILL_CONFLICT");
}

TEST(relay_layer_releases_the_starter_before_grounding_kill) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);
  CHECK(p.starterActive());

  const size_t before = fake::events.size();
  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setKill(p, true);

  CHECK(!p.starterActive());
  CHECK(p.killActive());

  size_t starterOff = SIZE_MAX, killOn = SIZE_MAX;
  for (size_t i = before; i < fake::events.size(); ++i) {
    const fake::Event& e = fake::events[i];
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    if (e.pin == PIN_RELAY_STARTER && e.value == inactiveLevel() && starterOff == SIZE_MAX) starterOff = i;
    if (e.pin == PIN_RELAY_KILL && e.value == activeLevel() && killOn == SIZE_MAX) killOn = i;
  }
  CHECK(starterOff != SIZE_MAX && killOn != SIZE_MAX);
  CHECK_MSG(starterOff < killOn, "kill closed before the starter opened");
}

TEST(simultaneous_starter_and_kill_flags_are_caught_and_faulted) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // Corrupt the internal picture the way a wild write or logic bug might.
  PumpTestAccess::corruptStarterFlag(p, true);
  PumpTestAccess::corruptKillFlag(p, true);

  tickAt(p, fake::nowMs + 1);

  CHECK_MSG(!p.starterActive(), "starter left active alongside kill");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "STARTER_KILL_CONFLICT");
}

TEST(spare_relay_activation_is_caught_and_faulted) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  PumpTestAccess::corruptSpareFlag(p);
  tickAt(p, fake::nowMs + 1);

  CHECK(!p.spareActive());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_SPARE], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "SPARE_ACTIVE");
}

TEST(spare_relay_cannot_be_commanded_active_through_the_relay_layer) {
  PumpController p;
  bootAt(p, 0);
  PumpTestAccess::setNow(p, fake::nowMs);
  PumpTestAccess::setSpare(p, true);   // deliberately asks for active
  CHECK_MSG(!p.spareActive(), "K4 accepted an activation request");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_SPARE], inactiveLevel());
  checkSpareNeverActive("explicit spare activation attempt");
}

TEST(fault_entered_while_engine_may_be_running_grounds_kill_then_releases) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);
  jumpBy(p, MAX_CRANK_MS + 1);   // -> FAULT via the starter backstop

  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_MSG(p.killActive(), "fault from a possibly-running state did not kill");
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());

  advanceBy(p, KILL_HOLD_MS);
  CHECK_MSG(!p.killActive(), "fault kill hold never released");
  CHECK_STREQ(toString(p.state()), "FAULT");   // stays until an operator resets
}

TEST(fault_is_reported_and_is_not_confirmed_running) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::FAULT);
  CHECK_STREQ(toString(p.engineStatus()), "UNKNOWN");
  CHECK(!p.runningConfirmed());
  CHECK_STREQ(toString(p.fault()), "STARTER_OVERRUN");
}

// ===========================================================================
// Quiescence (gates Wi-Fi reconnection)
// ===========================================================================

TEST(quiescence_is_false_during_every_timed_sequence) {
  const PumpState busy[] = {PumpState::CHOKING, PumpState::CRANKING,
                            PumpState::UNCHOKING, PumpState::STOPPING};
  for (PumpState s : busy) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    CHECK_MSG(!p.isQuiescent(), "reported quiescent during a timed sequence");
  }

  const PumpState quiet[] = {PumpState::UNKNOWN, PumpState::IDLE,
                             PumpState::RUNNING_ASSUMED, PumpState::RETRY_WAIT};
  for (PumpState s : quiet) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    CHECK_MSG(p.isQuiescent(), "reported busy while nothing was timed");
  }
}

TEST(quiescence_is_false_while_a_fault_kill_hold_is_running) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::FAULT);
  CHECK_MSG(!p.isQuiescent(), "quiescent while the fault kill relay was closed");
  advanceBy(p, KILL_HOLD_MS);
  CHECK(p.isQuiescent());
}

// ===========================================================================
// Idempotency
// ===========================================================================

TEST(duplicate_request_id_does_not_crank_the_engine_twice) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const CommandResult a = p.handleCommand(CommandType::START, "start-001", fake::nowMs);
  CHECK(a.accepted);
  CHECK(!a.duplicate);

  // The Pi retries because it never saw the response.
  const CommandResult b = p.handleCommand(CommandType::START, "start-001", fake::nowMs);
  CHECK_MSG(b.duplicate, "replayed request was not flagged as a duplicate");
  CHECK(b.accepted);
  CHECK_STREQ(toString(p.state()), "CHOKING");

  // Drive the sequence out and confirm the starter engaged exactly once.
  advanceBy(p, CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  CHECK_EQ(starterActivationCount(), 1);
}

TEST(duplicate_detection_is_scoped_to_the_command_type) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  p.handleCommand(CommandType::START, "shared-id", fake::nowMs);
  // Same ID, different command: this is a genuinely different operation.
  const CommandResult r = p.handleCommand(CommandType::STOP, "shared-id", fake::nowMs);
  CHECK(!r.duplicate);
  CHECK(r.accepted);
  CHECK_STREQ(toString(p.state()), "STOPPING");
}

TEST(duplicate_suppression_never_blocks_a_stop) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  p.handleCommand(CommandType::STOP, "stop-x", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS);
  CHECK_STREQ(toString(p.state()), "IDLE");

  // A replayed STOP is *labelled* a duplicate but is still executed. Skipping
  // a stop is dangerous; repeating one only re-grounds the kill circuit.
  const CommandResult dup = p.handleCommand(CommandType::STOP, "stop-x", fake::nowMs);
  CHECK(dup.duplicate);
  CHECK(dup.accepted);
  CHECK_MSG(p.killActive(), "a replayed STOP was silently swallowed");
  CHECK_STREQ(toString(p.state()), "STOPPING");

  advanceBy(p, KILL_HOLD_MS);
  const CommandResult fresh = p.handleCommand(CommandType::STOP, "stop-y", fake::nowMs);
  CHECK(!fresh.duplicate);
  CHECK(fresh.accepted);
  CHECK(p.killActive());
}

TEST(a_stale_stop_id_still_stops_an_engine_that_was_restarted_since) {
  // The dangerous case the exemption exists for: the Pi retransmits an old
  // STOP after a newer START has already begun cranking.
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  p.handleCommand(CommandType::STOP, "stop-1", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS);
  advanceBy(p, MIN_RECRANK_GAP_MS);

  p.handleCommand(CommandType::START, "start-2", fake::nowMs);
  advanceBy(p, CHOKE_PREP_MS);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  CHECK(p.starterActive());

  // The stale retransmission arrives.
  const CommandResult r = p.handleCommand(CommandType::STOP, "stop-1", fake::nowMs);
  CHECK(r.accepted);
  CHECK_MSG(!p.starterActive(), "a stale STOP left the engine cranking");
  CHECK_MSG(p.killActive(), "a stale STOP did not ground the kill circuit");
  CHECK_STREQ(toString(p.state()), "STOPPING");
}

TEST(a_replayed_stop_does_not_evict_the_idempotency_history) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  p.handleCommand(CommandType::START, "start-keep", fake::nowMs);
  const uint8_t slotsBefore = PumpTestAccess::recordSlotsUsed(p);

  // Hammer one STOP ID far more times than the ring has slots.
  for (int i = 0; i < 50; ++i) {
    p.handleCommand(CommandType::STOP, "stop-spam", fake::nowMs);
    advanceBy(p, KILL_HOLD_MS);
  }

  // The START ID must still be remembered.
  advanceBy(p, MIN_RECRANK_GAP_MS);
  const CommandResult r = p.handleCommand(CommandType::START, "start-keep", fake::nowMs);
  CHECK_MSG(r.duplicate, "a replayed STOP flushed the idempotency ring");
  CHECK(PumpTestAccess::recordSlotsUsed(p) >= slotsBefore);
}

TEST(idempotency_buffer_is_bounded_to_eight_entries) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // Nine accepted commands with distinct IDs. STOP is always accepted, so it
  // is the cleanest way to fill the ring.
  char id[32];
  for (int i = 0; i < 9; ++i) {
    snprintf(id, sizeof(id), "ring-%d", i);
    const CommandResult r = p.handleCommand(CommandType::STOP, id, fake::nowMs);
    CHECK(r.accepted);
    CHECK(!r.duplicate);
    advanceBy(p, KILL_HOLD_MS);
  }

  CHECK_EQ(PumpTestAccess::recordSlotsUsed(p), IDEMPOTENCY_SLOTS);

  // The most recent eight are still remembered.
  for (int i = 1; i < 9; ++i) {
    snprintf(id, sizeof(id), "ring-%d", i);
    const CommandResult r = p.handleCommand(CommandType::STOP, id, fake::nowMs);
    CHECK_MSG(r.duplicate, "a recent request ID was forgotten too early");
  }
}

TEST(rejected_commands_are_not_recorded_as_idempotency_entries) {
  PumpController p;
  bootAt(p, 0);

  // START from UNKNOWN is rejected.
  const CommandResult a = p.handleCommand(CommandType::START, "retry-me", fake::nowMs);
  CHECK(!a.accepted);
  CHECK(!a.duplicate);

  // After the operator resets, the same ID must be free to succeed.
  p.handleCommand(CommandType::RESET_IDLE, "reset-9", fake::nowMs);
  const CommandResult b = p.handleCommand(CommandType::START, "retry-me", fake::nowMs);
  CHECK_MSG(b.accepted, "a previously rejected ID was wrongly suppressed");
  CHECK(!b.duplicate);
}

TEST(commands_without_a_request_id_are_never_deduplicated) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const CommandResult a = p.handleCommand(CommandType::STOP, "", fake::nowMs);
  CHECK(a.accepted);
  CHECK(!a.duplicate);
  advanceBy(p, KILL_HOLD_MS);

  const CommandResult b = p.handleCommand(CommandType::STOP, "", fake::nowMs);
  CHECK(!b.duplicate);
  CHECK(b.accepted);

  const CommandResult c = p.handleCommand(CommandType::STOP, nullptr, fake::nowMs);
  CHECK(!c.duplicate);
  CHECK(c.accepted);
}

TEST(last_command_is_reported_for_status) {
  PumpController p;
  bootAt(p, 0);
  CHECK(!p.hasLastCommand());

  p.handleCommand(CommandType::RESET_IDLE, "reset-abc", fake::nowMs);
  CHECK(p.hasLastCommand());
  CHECK_STREQ(toString(p.lastCommandType()), "RESET_IDLE");
  CHECK_STREQ(p.lastCommandRequestId(), "reset-abc");
  CHECK(p.lastCommandAccepted());

  // A rejected command is still reported as the last command, flagged as not
  // accepted. start-failed is meaningless mid-sequence, so CHOKING rejects it.
  CHECK(driveToState(p, PumpState::CHOKING));
  const CommandResult r = p.handleCommand(CommandType::START_FAILED, "bad-1", fake::nowMs);
  CHECK(!r.accepted);
  CHECK_STREQ(toString(p.lastCommandType()), "START_FAILED");
  CHECK_STREQ(p.lastCommandRequestId(), "bad-1");
  CHECK(!p.lastCommandAccepted());
}

TEST(start_failed_from_idle_is_accepted_and_is_a_safe_no_op) {
  // Documented behaviour: start-failed is permitted from any state with no
  // timing sequence running. From a cooled-down IDLE it simply re-asserts
  // "all relays off" and resolves straight back to IDLE.
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const CommandResult r =
      p.handleCommand(CommandType::START_FAILED, "sf-idle", fake::nowMs);
  CHECK(r.accepted);
  CHECK_EQ(r.cooldownRemainingMs, 0u);
  CHECK(!p.starterActive());
  CHECK(!p.chokeActive());
  CHECK(!p.killActive());

  tickAt(p, fake::nowMs);
  CHECK_STREQ(toString(p.state()), "IDLE");
}

TEST(over_long_request_ids_are_truncated_safely_in_storage) {
  PumpController p;
  bootAt(p, 0);

  char longId[256];
  memset(longId, 'a', sizeof(longId) - 1);
  longId[sizeof(longId) - 1] = '\0';

  // The HTTP layer rejects these before they get here, but the controller
  // must not overflow if one arrives by another route.
  p.handleCommand(CommandType::RESET_IDLE, longId, fake::nowMs);
  CHECK_EQ(strlen(p.lastCommandRequestId()), REQUEST_ID_MAX_LEN);
}

// ===========================================================================
// millis() rollover
// ===========================================================================

TEST(full_start_sequence_is_correct_across_a_millis_rollover) {
  PumpController p;
  // Start close enough to 2^32 that every timer boundary wraps.
  const uint32_t start = 0xFFFFFF00u;
  bootAt(p, start);
  driveToState(p, PumpState::IDLE);

  p.handleCommand(CommandType::START, "roll-1", fake::nowMs);
  CHECK_STREQ(toString(p.state()), "CHOKING");

  advanceBy(p, CHOKE_PREP_MS - 1);
  CHECK_STREQ(toString(p.state()), "CHOKING");
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "CRANKING");

  advanceBy(p, CRANK_DURATION_MS - 1);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "UNCHOKING");

  advanceBy(p, UNCHOKE_DELAY_MS);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");

  // The clock genuinely wrapped during the sequence.
  CHECK_MSG(fake::nowMs < start, "test did not actually cross the rollover");
  CHECK_EQ(starterActiveTotalMs(), CRANK_DURATION_MS);
}

TEST(stop_and_cooldown_are_correct_across_a_millis_rollover) {
  PumpController p;
  bootAt(p, 0xFFFFF000u);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  p.handleCommand(CommandType::STOP, "roll-2", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS - 1);
  CHECK(p.killActive());
  advanceBy(p, 1);
  CHECK(!p.killActive());
  CHECK_STREQ(toString(p.state()), "IDLE");

  // Cooldown must still decrease monotonically to zero over the wrap.
  uint32_t prev = p.cooldownRemainingMs(fake::nowMs);
  CHECK(prev > 0);
  while (prev > 0) {
    advanceBy(p, 100);
    const uint32_t next = p.cooldownRemainingMs(fake::nowMs);
    CHECK_MSG(next <= prev, "cooldown increased across the rollover");
    prev = next;
  }
  CHECK_EQ(p.cooldownRemainingMs(fake::nowMs), 0u);
  const CommandResult r = p.handleCommand(CommandType::START, "roll-3", fake::nowMs);
  CHECK(r.accepted);
}

TEST(state_elapsed_time_is_rollover_safe) {
  PumpController p;
  bootAt(p, 0xFFFFFFF0u);
  driveToState(p, PumpState::IDLE);
  advanceBy(p, 1000);
  CHECK_EQ(p.stateElapsedMs(fake::nowMs), 1000u);
}

// ===========================================================================
// Engine status reporting
// ===========================================================================

TEST(running_confirmed_is_false_in_every_reachable_state) {
  for (PumpState s : kAllStates) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    CHECK_MSG(!p.runningConfirmed(),
              "running_confirmed was true; this build has no engine sensor");
  }
}

TEST(engine_status_never_claims_running_before_a_crank) {
  PumpController p;
  bootAt(p, 0);
  CHECK_STREQ(toString(p.engineStatus()), "UNKNOWN");
  driveToState(p, PumpState::IDLE);
  CHECK_STREQ(toString(p.engineStatus()), "STOPPED_ASSUMED");
  driveToState(p, PumpState::CHOKING);
  CHECK_STREQ(toString(p.engineStatus()), "STOPPED_ASSUMED");
  advanceBy(p, CHOKE_PREP_MS);
  CHECK_STREQ(toString(p.engineStatus()), "STARTING");
}
