// tests/test_support.cpp — see test_support.h.

#include "test_support.h"

const char* const kTestSecret = "s3cr3t-abcdefghijklmnopqrstuvwxyz-0123456789";

void tickAt(PumpController& p, uint32_t t) {
  fake::nowMs = t;
  p.tick(t);
}

void advanceBy(PumpController& p, uint32_t dt, uint32_t step) {
  if (step == 0) step = 1;
  uint32_t done = 0;
  while (done < dt) {
    const uint32_t chunk = (dt - done < step) ? (dt - done) : step;
    done += chunk;
    tickAt(p, static_cast<uint32_t>(fake::nowMs + chunk));
  }
}

void jumpBy(PumpController& p, uint32_t dt) {
  tickAt(p, static_cast<uint32_t>(fake::nowMs + dt));
}

void setWaterRaw(bool available) {
  // WATER_OK_LEVEL is LOW: the switch closes to ground when water is present,
  // so a broken wire (pull-up, HIGH) reads as "no water".
  fake::pinLevel[PIN_WATER_OK] = available ? LOW : HIGH;
}

void setWaterAvailable(PumpController& p, bool available) {
  setWaterRaw(available);
  advanceBy(p, WATER_DEBOUNCE_MS + 50, 25);
}

void bootAt(PumpController& p, uint32_t startMs) {
  fake::reset();
  fake::nowMs = startMs;
  // Water present by default; tests that care drive it explicitly.
  setWaterRaw(true);
  p.begin(startMs);
  // Let the interlock debounce settle, as it would in the first second of a
  // real boot. Tests exercising the debounce itself boot and drive it by hand.
  advanceBy(p, WATER_DEBOUNCE_MS + 50, 25);
}

bool driveToState(PumpController& p, PumpState target) {
  switch (target) {
    case PumpState::UNKNOWN:
      return p.state() == PumpState::UNKNOWN;

    case PumpState::IDLE:
      p.handleCommand(CommandType::RESET_IDLE, "fixture-idle", fake::nowMs);
      return p.state() == PumpState::IDLE;

    case PumpState::PRIMING:
      if (!driveToState(p, PumpState::IDLE)) return false;
      p.handleCommand(CommandType::START, "fixture-start", fake::nowMs);
      return p.state() == PumpState::PRIMING;

    case PumpState::CHOKING:
      if (INTAKE_VALVE_ENABLED) {
        if (!driveToState(p, PumpState::PRIMING)) return false;
        advanceBy(p, VALVE_PRIME_MS);
      } else {
        if (!driveToState(p, PumpState::IDLE)) return false;
        p.handleCommand(CommandType::START, "fixture-start", fake::nowMs);
      }
      return p.state() == PumpState::CHOKING;

    case PumpState::VALVE_CLOSING:
      if (!driveToState(p, PumpState::STOPPING)) return false;
      advanceBy(p, KILL_HOLD_MS);
      return p.state() == PumpState::VALVE_CLOSING;

    case PumpState::CRANKING:
      if (!driveToState(p, PumpState::CHOKING)) return false;
      advanceBy(p, CHOKE_PREP_MS);
      return p.state() == PumpState::CRANKING;

    case PumpState::UNCHOKING:
      if (!driveToState(p, PumpState::CRANKING)) return false;
      advanceBy(p, CRANK_DURATION_MS);
      return p.state() == PumpState::UNCHOKING;

    case PumpState::RUNNING_ASSUMED:
      if (!driveToState(p, PumpState::UNCHOKING)) return false;
      advanceBy(p, UNCHOKE_DELAY_MS);
      return p.state() == PumpState::RUNNING_ASSUMED;

    case PumpState::STOPPING:
      if (!driveToState(p, PumpState::RUNNING_ASSUMED)) return false;
      p.handleCommand(CommandType::STOP, "fixture-stop", fake::nowMs);
      return p.state() == PumpState::STOPPING;

    case PumpState::RETRY_WAIT:
      if (!driveToState(p, PumpState::RUNNING_ASSUMED)) return false;
      p.handleCommand(CommandType::START_FAILED, "fixture-failed", fake::nowMs);
      return p.state() == PumpState::RETRY_WAIT;

    case PumpState::FAULT: {
      // Provoke the MAX_CRANK_MS backstop by simulating a main loop that
      // stalls for longer than the absolute starter ceiling while cranking.
      if (!driveToState(p, PumpState::CRANKING)) return false;
      jumpBy(p, MAX_CRANK_MS + 1);
      return p.state() == PumpState::FAULT;
    }
  }
  return false;
}

std::string makeRequest(const char* method, const char* path,
                        const char* secret, const char* requestId) {
  std::string r;
  r += method;
  r += " ";
  r += path;
  r += " HTTP/1.1\r\nHost: 192.168.1.50:8080\r\n";
  if (secret != nullptr) {
    r += "X-Pump-Secret: ";
    r += secret;
    r += "\r\n";
  }
  if (requestId != nullptr) {
    r += "X-Request-ID: ";
    r += requestId;
    r += "\r\n";
  }
  r += "\r\n";
  return r;
}

std::string doRequest(PumpController& pump, const std::string& raw,
                      uint16_t& outStatus, uint32_t nowMs,
                      const char* secret) {
  RequestParser parser;
  for (char c : raw) {
    if (parser.feed(c) != ParseStatus::INCOMPLETE) {
      break;
    }
  }

  NetInfo net;
  net.connected = true;
  net.ip = "192.168.1.50";
  net.rssiDbm = -57;

  ResponsePlan plan;
  planResponse(plan, parser, pump, net, secret, nowMs, nowMs);
  parser.scrubSecret();

  outStatus = plan.status;
  // Log batches live in their own buffer; payload() picks the right one.
  return std::string(plan.payload(), plan.bodyLen);
}

void checkRelayPins(const PumpController& p, const char* where) {
  const uint8_t on = activeLevel();
  const uint8_t off = inactiveLevel();

  CHECK_MSG(fake::pinLevel[PIN_RELAY_STARTER] ==
                (p.starterActive() ? on : off),
            std::string("starter pin mismatch at ") + where);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_CHOKE] == (p.chokeActive() ? on : off),
            std::string("choke pin mismatch at ") + where);
  // Fail-safe inversion: kill asserted == relay de-energised.
  CHECK_MSG(fake::pinLevel[PIN_RELAY_KILL] == killPinLevelFor(p.killActive()),
            std::string("kill pin mismatch at ") + where);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_VALVE] == (p.valveActive() ? on : off),
            std::string("valve pin mismatch at ") + where);
}

void checkStarterNeverCrankedWithKillAsserted(const char* where) {
  // Replay the whole pin log and check the two lines were never in the
  // combination that would crank against a grounded ignition.
  bool starterOn = false;
  bool killEnergised = false;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    if (e.pin == PIN_RELAY_STARTER) {
      starterOn = (e.value == activeLevel());
    } else if (e.pin == PIN_RELAY_KILL) {
      killEnergised = (e.value == activeLevel());
    } else {
      continue;
    }
    const bool killAsserted = KILL_RELAY_FAIL_SAFE_NC ? !killEnergised
                                                      : killEnergised;
    CHECK_MSG(!(starterOn && killAsserted),
              std::string("starter engaged with kill asserted at ") + where);
  }
}
