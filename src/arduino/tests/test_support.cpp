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

void bootAt(PumpController& p, uint32_t startMs) {
  fake::reset();
  fake::nowMs = startMs;
  p.begin(startMs);
}

bool driveToState(PumpController& p, PumpState target) {
  switch (target) {
    case PumpState::UNKNOWN:
      return p.state() == PumpState::UNKNOWN;

    case PumpState::IDLE:
      p.handleCommand(CommandType::RESET_IDLE, "fixture-idle", fake::nowMs);
      return p.state() == PumpState::IDLE;

    case PumpState::CHOKING:
      if (!driveToState(p, PumpState::IDLE)) return false;
      p.handleCommand(CommandType::START, "fixture-start", fake::nowMs);
      return p.state() == PumpState::CHOKING;

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
  return std::string(plan.body, plan.bodyLen);
}

void checkRelayPins(const PumpController& p, const char* where) {
  const uint8_t on = activeLevel();
  const uint8_t off = inactiveLevel();

  CHECK_MSG(fake::pinLevel[PIN_RELAY_STARTER] ==
                (p.starterActive() ? on : off),
            std::string("starter pin mismatch at ") + where);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_CHOKE] == (p.chokeActive() ? on : off),
            std::string("choke pin mismatch at ") + where);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_KILL] == (p.killActive() ? on : off),
            std::string("kill pin mismatch at ") + where);
  CHECK_MSG(fake::pinLevel[PIN_RELAY_SPARE] == off,
            std::string("spare pin not inactive at ") + where);
}

void checkSpareNeverActive(const char* where) {
  const size_t idx = fake::firstWriteIndex(PIN_RELAY_SPARE, activeLevel());
  CHECK_MSG(idx == SIZE_MAX,
            std::string("K4/D5 was driven active at ") + where);
}
