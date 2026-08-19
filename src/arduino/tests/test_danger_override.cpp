// tests/test_danger_override.cpp — DANGER_OVERRIDE.
//
// The override exists so an operator can force a command past the firmware's
// judgement. That makes it the most dangerous feature in the project, so what
// it must NOT do matters more than what it does.
//
// The dividing line under test:
//
//   PRECONDITION interlocks -- overridable. These encode the firmware's
//   opinion that a start is unlikely to be wanted right now: not IDLE,
//   cooldown still running, prime dwell incomplete, relays not at rest,
//   water absent. On a fire pump the operator may legitimately disagree.
//
//   DESTRUCTIVE-LIMIT backstops -- NOT overridable, on any path. MAX_CRANK_MS,
//   the starter/kill exclusion, shutdown on a commanded-shut intake, and the
//   fail-safe kill. An override that relaxed these would not be more powerful,
//   just broken.

#include "test_support.h"

namespace {

constexpr bool kOverride = true;
constexpr bool kNormal = false;

// Was the starter ever driven active while the valve pin was inactive?
// Deliberately re-derived from the recorded pin events rather than from any
// controller flag: the question is electrical, not what the firmware believes.
bool crankedWithIntakeShut() {
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
    if (starterOn && !valveOpen) return true;
  }
  return false;
}

uint8_t lastStarterLevel() {
  uint8_t level = inactiveLevel();
  for (const fake::Event& e : fake::events) {
    if (e.kind == fake::EventKind::DIGITAL_WRITE && e.pin == PIN_RELAY_STARTER) {
      level = e.value;
    }
  }
  return level;
}

// Longest continuous stretch the starter pin was held active, in ms.
uint32_t longestCrankMs() {
  bool on = false;
  uint32_t onAt = 0;
  uint32_t worst = 0;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool active = (e.value == activeLevel());
    if (active && !on) {
      on = true;
      onAt = e.at;
    } else if (!active && on) {
      on = false;
      const uint32_t held = static_cast<uint32_t>(e.at - onAt);
      if (held > worst) worst = held;
    }
  }
  return worst;
}

uint32_t firstCrankAt() {
  for (const fake::Event& e : fake::events) {
    if (e.kind == fake::EventKind::DIGITAL_WRITE &&
        e.pin == PIN_RELAY_STARTER && e.value == activeLevel()) {
      return e.at;
    }
  }
  return UINT32_MAX;
}

CommandResult startWith(PumpController& p, uint32_t now, bool override,
                        const char* id = nullptr) {
  return p.handleCommand(CommandType::START, id, now, nullptr, override);
}

// Long enough for any start sequence to reach and leave the starter.
uint32_t fullSequenceMs() {
  return VALVE_PRIME_MS + CHOKE_PREP_MS + CRANK_DURATION_MS +
         UNCHOKE_DELAY_MS + 500;
}

}  // namespace

// ---------------------------------------------------------------------------
// It does what it claims
// ---------------------------------------------------------------------------

TEST(override_forces_a_start_from_states_that_would_normally_refuse) {
  const PumpState blocked[] = {
      PumpState::UNKNOWN,
      PumpState::RETRY_WAIT,
      PumpState::FAULT,
      PumpState::RUNNING_ASSUMED,
  };
  for (PumpState s : blocked) {
    PumpController p;
    bootAt(p, 1000);
    if (!driveToState(p, s)) continue;

    const uint32_t now = fake::nowMs;
    const CommandResult refused = startWith(p, now, kNormal, "normal-1");
    CHECK_MSG(!refused.accepted,
              "a normal START was accepted from a blocked state");
    CHECK(refused.httpStatus == 409);

    const CommandResult forced = startWith(p, now, kOverride, "forced-1");
    CHECK_MSG(forced.accepted, "the override did not force the start through");
    CHECK(forced.httpStatus == 202);
  }
}

TEST(override_forces_a_start_during_the_recrank_cooldown) {
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  CHECK(p.handleCommand(CommandType::START, "s1", fake::nowMs).accepted);
  advanceBy(p, fullSequenceMs());
  p.handleCommand(CommandType::START_FAILED, "f1", fake::nowMs);

  const uint32_t now = fake::nowMs;
  CHECK_MSG(p.cooldownRemainingMs(now) > 0, "no cooldown to test against");
  CHECK_MSG(!startWith(p, now, kNormal, "n2").accepted,
            "a normal START ignored the recrank cooldown");
  CHECK_MSG(startWith(p, now, kOverride, "o2").accepted,
            "the override did not bypass the recrank cooldown");
}

TEST(override_skips_the_prime_dwell) {
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  fake::events.clear();
  const uint32_t startedAt = fake::nowMs;
  CHECK(startWith(p, startedAt, kOverride, "o3").accepted);
  advanceBy(p, CHOKE_PREP_MS + CRANK_DURATION_MS + 200);

  const uint32_t crankAt = firstCrankAt();
  CHECK_MSG(crankAt != UINT32_MAX, "the overridden start never cranked");
  CHECK_MSG(static_cast<uint32_t>(crankAt - startedAt) < VALVE_PRIME_MS,
            "the override still waited out the full prime dwell");
}

TEST(a_normal_start_still_observes_the_full_prime_dwell) {
  // The counterpart to the test above: proving the override shortens things
  // is only meaningful if the default path is proven not to.
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  fake::events.clear();
  const uint32_t startedAt = fake::nowMs;
  CHECK(p.handleCommand(CommandType::START, "n3", startedAt).accepted);
  advanceBy(p, fullSequenceMs());

  const uint32_t crankAt = firstCrankAt();
  CHECK_MSG(crankAt != UINT32_MAX, "a normal start never cranked");
  CHECK_MSG(static_cast<uint32_t>(crankAt - startedAt) >= VALVE_PRIME_MS,
            "a normal start cranked before the prime dwell elapsed");
}

// ---------------------------------------------------------------------------
// It does NOT relax the destructive limits
// ---------------------------------------------------------------------------

TEST(override_never_holds_the_starter_beyond_max_crank) {
  // The most important test in this file. An overridden start asking for a
  // crank far longer than the ceiling must still be cut off at MAX_CRANK_MS.
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  fake::events.clear();
  StartTimings t;
  t.crankMs = MAX_CRANK_MS * 4;  // clamp() must bring this down
  CHECK(p.handleCommand(CommandType::START, "o4", fake::nowMs, &t, kOverride).accepted);
  advanceBy(p, MAX_CRANK_MS * 3);

  CHECK_MSG(longestCrankMs() <= MAX_CRANK_MS,
            "an overridden start held the starter past MAX_CRANK_MS");
  CHECK_MSG(lastStarterLevel() == inactiveLevel(),
            "the starter was left energised after an overridden start");
}

TEST(override_never_cranks_with_the_intake_commanded_shut) {
  // The override skips the prime DWELL but not the valve POSITION. Cranking
  // into a shut intake is the dry-run case the whole design exists to stop.
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  fake::events.clear();
  CHECK(startWith(p, fake::nowMs, kOverride, "o5").accepted);

  // Force the intake shut mid-sequence, as a failed actuator would.
  PumpTestAccess::corruptValveFlag(p, false);
  advanceBy(p, CHOKE_PREP_MS + CRANK_DURATION_MS + 500);

  CHECK_MSG(!crankedWithIntakeShut(),
            "an overridden start cranked with the intake commanded shut");
}

TEST(override_never_energises_the_starter_while_the_kill_is_asserted) {
  const PumpState from[] = {
      PumpState::UNKNOWN,
      PumpState::FAULT,
      PumpState::RETRY_WAIT,
      PumpState::RUNNING_ASSUMED,
  };
  for (PumpState s : from) {
    PumpController p;
    bootAt(p, 1000);
    if (!driveToState(p, s)) continue;

    fake::events.clear();
    startWith(p, fake::nowMs, kOverride, nullptr);
    advanceBy(p, fullSequenceMs());
    checkStarterNeverCrankedWithKillAsserted("overridden start");
  }
}

TEST(override_does_not_defeat_the_max_crank_backstop_under_a_stalled_loop) {
  // Combines the two worst cases: an override in flight and a main loop that
  // stops ticking. The first tick after the stall must still shed the starter.
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  fake::events.clear();
  CHECK(startWith(p, fake::nowMs, kOverride, "o6").accepted);
  advanceBy(p, CHOKE_PREP_MS + 100);

  jumpBy(p, MAX_CRANK_MS * 5);
  CHECK_MSG(lastStarterLevel() == inactiveLevel(),
            "a stalled loop under an override left the starter energised");
}

// ---------------------------------------------------------------------------
// It cannot leak
// ---------------------------------------------------------------------------

TEST(override_does_not_survive_into_a_later_normal_start) {
  if (!INTAKE_VALVE_ENABLED) return;

  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  CHECK(startWith(p, fake::nowMs, kOverride, "o7").accepted);
  CHECK_MSG(p.overrideActive(), "the override did not latch for its sequence");

  p.handleCommand(CommandType::STOP, "stop-7", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS + 1000);
  CHECK_MSG(!p.overrideActive(), "the override outlived its own sequence");

  // Far enough ahead that the recrank cooldown is gone, leaving only the
  // prime dwell to prove.
  advanceBy(p, MIN_RECRANK_GAP_MS * 2);
  CHECK(p.state() == PumpState::IDLE);

  fake::events.clear();
  const uint32_t now = fake::nowMs;
  CHECK(p.handleCommand(CommandType::START, "n7", now).accepted);
  advanceBy(p, fullSequenceMs());

  const uint32_t crankAt = firstCrankAt();
  CHECK_MSG(crankAt != UINT32_MAX, "the follow-up start never cranked");
  CHECK_MSG(static_cast<uint32_t>(crankAt - now) >= VALVE_PRIME_MS,
            "a normal start inherited the earlier override's dwell skip");
}

TEST(override_is_cleared_by_every_state_outside_its_sequence) {
  const PumpState ends[] = {
      PumpState::IDLE,
      PumpState::FAULT,
      PumpState::RETRY_WAIT,
      PumpState::STOPPING,
  };
  for (PumpState s : ends) {
    PumpController p;
    bootAt(p, 1000);
    CHECK(driveToState(p, PumpState::IDLE));
    CHECK(startWith(p, fake::nowMs, kOverride, "o8").accepted);
    CHECK(p.overrideActive());

    if (!driveToState(p, s)) continue;
    CHECK_MSG(!p.overrideActive(),
              "the override survived into a state outside its sequence");
  }
}

TEST(override_is_cleared_by_a_reboot) {
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));
  CHECK(startWith(p, fake::nowMs, kOverride, "o9").accepted);
  CHECK(p.overrideActive());
  CHECK(p.overrideCount() == 1);

  bootAt(p, 1000);
  CHECK_MSG(!p.overrideActive(), "an override survived a reboot");
  CHECK_MSG(p.overrideCount() == 0, "the override count survived a reboot");
}

TEST(override_use_is_counted_exactly_once_per_accepted_command) {
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));
  CHECK(p.overrideCount() == 0);

  CHECK(startWith(p, fake::nowMs, kOverride, "c1").accepted);
  CHECK_MSG(p.overrideCount() == 1, "an accepted override was not counted");

  // A replay of the same request ID must not inflate the count.
  startWith(p, fake::nowMs, kOverride, "c1");
  CHECK_MSG(p.overrideCount() == 1, "a replayed override was counted twice");

  // A normal command must not touch it.
  p.handleCommand(CommandType::STOP, "c2", fake::nowMs);
  CHECK_MSG(p.overrideCount() == 1, "a normal command changed the count");
}

TEST(a_rejected_override_is_not_counted) {
  // Nothing currently rejects an overridden START, but the counter must mean
  // "an interlock was actually bypassed" rather than "someone asked".
  PumpController p;
  bootAt(p, 1000);
  CHECK(driveToState(p, PumpState::IDLE));

  // MAINT commands are unreachable when the maintenance API is compiled out;
  // when compiled in they are permitted. Either way the count must match the
  // number of ACCEPTED overridden commands, which is what this asserts.
  const CommandResult r =
      p.handleCommand(CommandType::MAINT_CHOKE_ON, "m1", fake::nowMs, nullptr,
                      kOverride);
  CHECK(p.overrideCount() == (r.accepted ? 1u : 0u));
}

// ---------------------------------------------------------------------------
// The default path is unchanged
// ---------------------------------------------------------------------------

TEST(a_normal_command_is_unaffected_by_the_override_existing) {
  const PumpState blocked[] = {
      PumpState::UNKNOWN,
      PumpState::FAULT,
      PumpState::RETRY_WAIT,
  };
  for (PumpState s : blocked) {
    PumpController p;
    bootAt(p, 1000);
    if (!driveToState(p, s)) continue;

    const CommandResult r = p.handleCommand(CommandType::START, "n9", fake::nowMs);
    CHECK_MSG(!r.accepted, "a normal START was accepted from a blocked state");
    CHECK(r.httpStatus == 409);
    CHECK_MSG(!p.overrideActive(), "a normal command set the override flag");
    CHECK_MSG(p.overrideCount() == 0, "a normal command was counted");
  }
}
