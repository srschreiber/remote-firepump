// tests/test_invariants.cpp — property-based testing.
//
// The example-based tests prove the sequences we thought to write down. These
// tests attack the same code with millions of randomised command/timing
// permutations and assert the safety invariants after EVERY single step.
//
// Everything is driven by a fixed-seed xorshift PRNG, so a failure is exactly
// reproducible from the seed printed in the assertion message.

#include "test_support.h"

#include <stdio.h>

namespace {

// Deterministic, portable PRNG. std::mt19937 would work too, but this keeps
// the sequence identical across compilers and standard libraries.
class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed ? seed : 1u) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  uint32_t below(uint32_t n) { return n ? (next() % n) : 0u; }
  bool chance(uint32_t oneIn) { return below(oneIn) == 0; }

 private:
  uint32_t s_;
};

// Tracks the starter pin directly from the event log so the invariant is
// checked against observed electrical behaviour, not internal flags.
class StarterWatch {
 public:
  void scan(size_t& cursor) {
    for (; cursor < fake::events.size(); ++cursor) {
      const fake::Event& e = fake::events[cursor];
      if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
      if (e.pin == PIN_RELAY_STARTER) {
        const bool on = (e.value == activeLevel());
        if (on && !on_) {
          on_ = true;
          onAt_ = e.at;
        } else if (!on && on_) {
          on_ = false;
          const uint32_t d = static_cast<uint32_t>(e.at - onAt_);
          if (d > longest_) longest_ = d;
        }
      } else if (e.pin == PIN_RELAY_VALVE) {
        valveOpen_ = (e.value == activeLevel());
      }
      // Re-evaluated on every edge of either line.
      if (on_ && !valveOpen_ && INTAKE_VALVE_ENABLED) {
        crankedWithIntakeShut_ = true;
      }
    }
  }

  uint32_t currentOnMs(uint32_t now) const {
    return on_ ? static_cast<uint32_t>(now - onAt_) : 0u;
  }
  uint32_t longest() const { return longest_; }
  bool crankedWithIntakeShut() const { return crankedWithIntakeShut_; }

 private:
  bool     on_ = false;
  uint32_t onAt_ = 0;
  uint32_t longest_ = 0;
  bool     valveOpen_ = false;
  bool     crankedWithIntakeShut_ = false;
};

// The invariants that must hold at absolutely every observable moment.
// `strictTiming` is false for sessions that deliberately stall the simulated
// main loop. A stall physically holds whatever the relay was last commanded
// to, which no firmware can undo retroactively; those sessions therefore
// assert the achievable guarantee (see invariant 3) rather than a wall-clock
// bound the loop was never given the chance to honour.
void checkInvariants(const PumpController& p, const StarterWatch& w,
                     uint32_t now, uint32_t seed, uint32_t step,
                     bool strictTiming) {
  char ctx[128];
  snprintf(ctx, sizeof(ctx), "seed=%lu step=%lu",
           static_cast<unsigned long>(seed), static_cast<unsigned long>(step));

  // 1. Starter and kill are never energised together.
  CHECK_MSG(!(p.starterActive() && p.killActive()),
            std::string("starter and kill both active; ") + ctx);
  // Physically: the starter is never energised while K3 is de-energised,
  // because a de-energised K3 has its NC contact closed onto engine ground.
  CHECK_MSG(!(fake::pinLevel[PIN_RELAY_STARTER] == activeLevel() &&
              fake::pinLevel[PIN_RELAY_KILL] == killPinLevelFor(true)),
            std::string("starter energised with the kill wire grounded; ") + ctx);

  // 2a. The starter is never engaged with the intake shut, and never with the
  //     kill asserted. These are the two preconditions for cranking.
  CHECK_MSG(!(p.starterActive() && !p.valveActive() && INTAKE_VALVE_ENABLED),
            std::string("cranking with the intake shut; ") + ctx);
  CHECK_MSG(!(p.starterActive() && p.killActive()),
            std::string("cranking with the kill asserted; ") + ctx);
  CHECK_MSG(!w.crankedWithIntakeShut(),
            std::string("cranked with the intake shut at some point; ") + ctx);

  // 2b. Whenever the engine has actually been cranked and could be running,
  //     the intake is open.
  //
  //     STOPPING is excluded deliberately: it is reachable from ANY state,
  //     including IDLE where the engine was never started and the intake is
  //     legitimately shut. The guarantee that matters -- that the intake is
  //     never shut *while* the engine runs -- is a property of the
  //     transition, enforced in setValveRelay(), and is asserted directly in
  //     test_valve_water.cpp.
  if (INTAKE_VALVE_ENABLED) {
    const bool startedByUs = (p.state() == PumpState::CRANKING ||
                              p.state() == PumpState::UNCHOKING ||
                              p.state() == PumpState::RUNNING_ASSUMED);
    CHECK_MSG(!(startedByUs && !p.valveActive()),
              std::string("running with the intake shut; ") + ctx);
  }

  // 2c. The kill pin always matches the fail-safe wiring inversion.
  CHECK_MSG(fake::pinLevel[PIN_RELAY_KILL] == killPinLevelFor(p.killActive()),
            std::string("kill pin does not match the asserted state; ") + ctx);

  // 3. The core starter guarantee: at the end of every tick the starter is
  //    either released, or has been engaged for strictly less than the
  //    absolute ceiling. This holds even when the loop stalls, because the
  //    first tick after the stall must shed it.
  CHECK_MSG(!p.starterActive() || w.currentOnMs(now) < MAX_CRANK_MS,
            std::string("starter still engaged past MAX_CRANK_MS after a tick; ") + ctx);

  // 3b. With normal scheduling, no crank ever physically exceeds the ceiling.
  if (strictTiming) {
    CHECK_MSG(w.longest() <= MAX_CRANK_MS,
              std::string("a crank exceeded MAX_CRANK_MS; ") + ctx);
    CHECK_MSG(w.longest() <= 5000,
              std::string("a crank exceeded five seconds; ") + ctx);
  }

  // 4. The commanded pin levels always agree with the reported relay state.
  CHECK_MSG(fake::pinLevel[PIN_RELAY_STARTER] ==
                (p.starterActive() ? activeLevel() : inactiveLevel()),
            std::string("starter pin/state disagreement; ") + ctx);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_CHOKE] ==
                (p.chokeActive() ? activeLevel() : inactiveLevel()),
            std::string("choke pin/state disagreement; ") + ctx);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_VALVE] ==
                (p.valveActive() ? activeLevel() : inactiveLevel()),
            std::string("valve pin/state disagreement; ") + ctx);

  // 5. The engine is never reported as confirmed running.
  CHECK_MSG(!p.runningConfirmed(),
            std::string("running_confirmed became true; ") + ctx);

  // 6. Quiescence means no relay timing is pending, which is what permits a
  //    blocking Wi-Fi reconnect. The kill and valve are excluded by design --
  //    see PumpController::isQuiescent().
  if (p.isQuiescent()) {
    CHECK_MSG(!p.starterActive() && !p.chokeActive(),
              std::string("quiescent while a timed relay was energised; ") + ctx);
  }
}

const CommandType kCommands[] = {CommandType::START, CommandType::STOP,
                                 CommandType::START_FAILED,
                                 CommandType::RESET_IDLE};

// One randomised session. Returns the number of commands issued.
uint32_t fuzzSession(uint32_t seed, uint32_t iterations, uint32_t startClock,
                     bool allowLargeJumps) {
  Rng rng(seed);
  PumpController p;
  bootAt(p, startClock);

  StarterWatch watch;
  size_t cursor = 0;
  uint32_t commandsIssued = 0;

  // Realistic loop periods, plus occasional long stalls when enabled.
  const uint32_t smallSteps[] = {0, 1, 1, 1, 2, 5, 13, 100, 499, 1000};

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t dt = smallSteps[rng.below(
        static_cast<uint32_t>(sizeof(smallSteps) / sizeof(smallSteps[0])))];
    if (allowLargeJumps && rng.chance(500)) {
      dt = 3000 + rng.below(90000);   // simulate a badly stalled main loop
    }

    fake::nowMs = static_cast<uint32_t>(fake::nowMs + dt);
    p.tick(fake::nowMs);
    watch.scan(cursor);
    checkInvariants(p, watch, fake::nowMs, seed, i, !allowLargeJumps);

    // Roughly one iteration in six carries a command.
    if (rng.chance(6)) {
      const CommandType cmd =
          kCommands[rng.below(static_cast<uint32_t>(sizeof(kCommands) /
                                                    sizeof(kCommands[0])))];
      // A small ID pool guarantees frequent duplicate replays.
      char id[24];
      if (rng.chance(8)) {
        id[0] = '\0';   // no request ID at all
      } else {
        snprintf(id, sizeof(id), "id-%lu",
                 static_cast<unsigned long>(rng.below(12)));
      }

      const CommandResult r = p.handleCommand(cmd, id, fake::nowMs);
      ++commandsIssued;
      watch.scan(cursor);
      checkInvariants(p, watch, fake::nowMs, seed, i, !allowLargeJumps);

      // STOP is never rejected, from any state, ever.
      if (cmd == CommandType::STOP) {
        CHECK_MSG(r.accepted, "a STOP was rejected during fuzzing");
        // Including replays: STOP is exempt from duplicate suppression.
        CHECK_MSG(p.killActive(), "a STOP did not ground the kill circuit");
        CHECK_MSG(!p.starterActive(), "STOP left the starter energised");
        CHECK_MSG(!p.chokeActive(), "STOP left the choke energised");
      }

      // START is only ever accepted out of IDLE with no cooldown left.
      if (cmd == CommandType::START && r.accepted && !r.duplicate) {
        CHECK_MSG(p.state() == (INTAKE_VALVE_ENABLED ? PumpState::PRIMING
                                                     : PumpState::CHOKING),
                  "an accepted START did not begin the start sequence");
      }
    }
  }

  // Finally, run the machine out and confirm it settles with everything off.
  for (uint32_t i = 0; i < 4000; ++i) {
    fake::nowMs = static_cast<uint32_t>(fake::nowMs + 50);
    p.tick(fake::nowMs);
    watch.scan(cursor);
    checkInvariants(p, watch, fake::nowMs, seed, 100000 + i, !allowLargeJumps);
  }
  // Settling means no timed sequence is pending -- NOT that everything is
  // off. RUNNING_ASSUMED is a legitimate resting state that holds the kill
  // released and the intake open indefinitely, by design.
  CHECK_MSG(!p.starterActive(), "starter still energised after settling");
  CHECK_MSG(!p.chokeActive(), "choke still energised after settling");

  if (p.state() == PumpState::RUNNING_ASSUMED) {
    CHECK_MSG(!p.killActive(), "RUNNING_ASSUMED with the engine inhibited");
    if (INTAKE_VALVE_ENABLED) {
      CHECK_MSG(p.valveActive(), "RUNNING_ASSUMED with the intake shut");
    }
  } else if (p.state() == PumpState::IDLE ||
             p.state() == PumpState::RETRY_WAIT ||
             p.state() == PumpState::UNKNOWN) {
    // At rest the engine must be inhibited and the intake shut.
    CHECK_MSG(p.killActive(), "settled at rest without inhibiting the engine");
    CHECK_MSG(fake::pinLevel[PIN_RELAY_KILL] == killPinLevelFor(true),
              "the kill pin was not at its fail-safe level at rest");
    if (INTAKE_VALVE_ENABLED) {
      CHECK_MSG(!p.valveActive(), "intake left open at rest");
    }
  }

  return commandsIssued;
}

}  // namespace

// ===========================================================================

TEST(fuzz_invariants_hold_under_randomised_command_and_timing_sequences) {
  uint32_t total = 0;
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    total += fuzzSession(seed, 3000, 0, /*allowLargeJumps=*/false);
  }
  CHECK_MSG(total > 5000, "the fuzzer did not issue a meaningful command load");
}

TEST(fuzz_invariants_hold_when_the_main_loop_stalls_unpredictably) {
  // Long stalls take the state machine down the MAX_CRANK_MS / MAX_CHOKE_MS
  // backstop paths that normal scheduling never reaches.
  for (uint32_t seed = 101; seed <= 130; ++seed) {
    fuzzSession(seed, 3000, 0, /*allowLargeJumps=*/true);
  }
}

TEST(fuzz_invariants_hold_across_the_millis_rollover) {
  // Start just short of 2^32 so every session wraps the clock repeatedly.
  for (uint32_t seed = 201; seed <= 230; ++seed) {
    fuzzSession(seed, 3000, 0xFFFF0000u, /*allowLargeJumps=*/true);
  }
}

TEST(fuzz_stop_always_wins_from_any_reachable_situation) {
  // After any randomised prelude, a STOP must be accepted and must
  // immediately shed the starter and choke while grounding kill.
  for (uint32_t seed = 301; seed <= 400; ++seed) {
    Rng rng(seed);
    PumpController p;
    bootAt(p, rng.next());

    for (uint32_t i = 0; i < 60; ++i) {
      fake::nowMs = static_cast<uint32_t>(fake::nowMs + rng.below(2500));
      p.tick(fake::nowMs);
      if (rng.chance(3)) {
        char id[24];
        snprintf(id, sizeof(id), "pre-%lu",
                 static_cast<unsigned long>(rng.below(6)));
        p.handleCommand(
            kCommands[rng.below(static_cast<uint32_t>(
                sizeof(kCommands) / sizeof(kCommands[0])))],
            id, fake::nowMs);
      }
    }

    const CommandResult r = p.handleCommand(CommandType::STOP, "the-stop", fake::nowMs);
    CHECK_MSG(r.accepted, "STOP was rejected after a randomised prelude");
    CHECK_MSG(!p.starterActive(), "STOP left the starter energised");
    CHECK_MSG(!p.chokeActive(), "STOP left the choke energised");
    CHECK_MSG(p.killActive(), "STOP did not ground the kill circuit");
    CHECK_MSG(p.state() == PumpState::STOPPING, "STOP did not enter STOPPING");
    CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
    // Kill asserted == K3 de-energised == NC contact grounding the ignition.
    CHECK_EQ(fake::pinLevel[PIN_RELAY_KILL], killPinLevelFor(true));

    // And it always completes: kill hold, then the intake close delay, landing
    // in IDLE with the engine still inhibited and the intake shut.
    advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS + 100, 7);
    CHECK_MSG(p.state() == PumpState::IDLE,
              std::string("STOP did not settle back to IDLE; ended in ") +
                  toString(p.state()));
    CHECK_MSG(p.killActive(), "the kill was released after the stop completed");
    if (INTAKE_VALVE_ENABLED) {
      CHECK_MSG(!p.valveActive(), "STOP left the intake open");
    }
  }
}

TEST(fuzz_the_http_parser_never_crashes_or_leaks_authorisation) {
  // Random byte soup, random header-ish lines, and mutated valid requests.
  // Nothing must ever produce a 2xx without the exact secret.
  const std::string valid =
      std::string("POST /v1/start HTTP/1.1\r\nHost: h\r\nX-Pump-Secret: ") +
      kTestSecret + "\r\nX-Request-ID: fuzz-1\r\n\r\n";

  uint32_t authorised = 0;
  uint32_t rejected = 0;

  for (uint32_t seed = 501; seed <= 900; ++seed) {
    Rng rng(seed);

    std::string raw;
    const uint32_t mode = rng.below(4);
    if (mode == 0) {
      // Pure random bytes.
      const uint32_t n = rng.below(3000);
      for (uint32_t i = 0; i < n; ++i) {
        raw += static_cast<char>(rng.below(256));
      }
    } else if (mode == 1) {
      // Random bytes that are at least line-structured.
      const uint32_t lines = rng.below(40);
      for (uint32_t i = 0; i < lines; ++i) {
        const uint32_t len = rng.below(300);
        for (uint32_t j = 0; j < len; ++j) {
          raw += static_cast<char>(32 + rng.below(95));
        }
        raw += "\r\n";
      }
      raw += "\r\n";
    } else {
      // A valid request with random single-byte mutations.
      raw = valid;
      const uint32_t muts = 1 + rng.below(6);
      for (uint32_t i = 0; i < muts && !raw.empty(); ++i) {
        const uint32_t pos = rng.below(static_cast<uint32_t>(raw.size()));
        raw[pos] = static_cast<char>(rng.below(256));
      }
    }

    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);

    uint16_t status = 0;
    const std::string body = doRequest(p, raw, status, fake::nowMs);

    // A 2xx is only ever legitimate if the untouched secret survived.
    if (status >= 200 && status < 300) {
      ++authorised;
      CHECK_MSG(raw.find(std::string("X-Pump-Secret: ") + kTestSecret) !=
                    std::string::npos,
                "a request without the exact secret produced a 2xx");
    } else {
      ++rejected;
      CHECK_MSG(status == 400 || status == 401 || status == 404 ||
                    status == 405 || status == 409 || status == 413 ||
                    status == 500,
                "the parser produced an undocumented status code");
    }

    // The body must always be a plausible JSON object and never leak input.
    CHECK_MSG(!body.empty(), "an empty response body was produced");
    CHECK_MSG(body.front() == '{' && body.back() == '}',
              "the response body was not a JSON object");
    CHECK_NOT_CONTAINS(body, "\n");
    CHECK_NOT_CONTAINS(body, "\r");
  }

  CHECK_MSG(rejected > 300, "the parser fuzzer barely rejected anything");
  // Some mutations land harmlessly (e.g. inside the User-Agent), so a handful
  // of authorised requests is expected and healthy.
  CHECK_MSG(authorised > 0, "no fuzzed request was ever authorised; check the corpus");
}

TEST(fuzz_incremental_feeding_matches_whole_request_feeding) {
  // The parser is fed one byte at a time by the real server, interleaved with
  // state-machine ticks. Chunk boundaries must not change the outcome.
  const char* requests[] = {
      "GET /v1/status HTTP/1.1\r\nX-Pump-Secret: abc\r\n\r\n",
      "POST /v1/start HTTP/1.1\r\nX-Request-ID: a-1\r\nX-Pump-Secret: abc\r\n\r\n",
      "POST /v1/stop HTTP/1.1\r\n\r\n",
      "BOGUS\r\n\r\n",
      "GET /v1/status HTTP/1.1\nX-Pump-Secret: abc\n\n",
  };

  for (const char* raw : requests) {
    RequestParser whole;
    ParseStatus wholeStatus = ParseStatus::INCOMPLETE;
    for (const char* c = raw; *c; ++c) {
      wholeStatus = whole.feed(*c);
      if (wholeStatus != ParseStatus::INCOMPLETE) break;
    }

    // Now the same bytes with a tick between every single one.
    PumpController p;
    bootAt(p, 0);
    RequestParser drip;
    ParseStatus dripStatus = ParseStatus::INCOMPLETE;
    for (const char* c = raw; *c; ++c) {
      tickAt(p, fake::nowMs + 3);
      dripStatus = drip.feed(*c);
      if (dripStatus != ParseStatus::INCOMPLETE) break;
    }

    CHECK_MSG(wholeStatus == dripStatus, raw);
    CHECK_MSG(whole.errorStatus() == drip.errorStatus(), raw);
    CHECK_STREQ(whole.request().path, drip.request().path);
    CHECK_MSG(whole.request().method == drip.request().method, raw);
    CHECK_STREQ(whole.request().secret, drip.request().secret);
    CHECK_STREQ(whole.request().requestId, drip.request().requestId);
  }
}

TEST(fuzz_idempotency_never_permits_a_second_crank_for_one_request_id) {
  // Hammer a single request ID from many states and timings; the starter must
  // engage at most once per distinct accepted START.
  for (uint32_t seed = 901; seed <= 960; ++seed) {
    Rng rng(seed);
    PumpController p;
    bootAt(p, rng.next());
    driveToState(p, PumpState::IDLE);

    int acceptedNonDuplicateStarts = 0;
    int activations = 0;
    bool on = false;
    size_t cursor = 0;

    for (uint32_t i = 0; i < 400; ++i) {
      fake::nowMs = static_cast<uint32_t>(fake::nowMs + rng.below(900));
      p.tick(fake::nowMs);

      if (rng.chance(3)) {
        const CommandResult r =
            p.handleCommand(CommandType::START, "the-only-id", fake::nowMs);
        if (r.accepted && !r.duplicate) ++acceptedNonDuplicateStarts;
      }
      if (rng.chance(25)) {
        p.handleCommand(CommandType::STOP, "stop-id", fake::nowMs);
      }

      for (; cursor < fake::events.size(); ++cursor) {
        const fake::Event& e = fake::events[cursor];
        if (e.kind != fake::EventKind::DIGITAL_WRITE ||
            e.pin != PIN_RELAY_STARTER) {
          continue;
        }
        const bool nowOn = (e.value == activeLevel());
        if (nowOn && !on) ++activations;
        on = nowOn;
      }
    }

    CHECK_MSG(acceptedNonDuplicateStarts <= 1,
              "one request ID was accepted as a fresh START more than once");
    CHECK_MSG(activations <= 1,
              "one request ID produced more than one starter engagement");
  }
}
