// pump_controller.cpp — see pump_controller.h.

#include "pump_controller.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Name tables
// ---------------------------------------------------------------------------

const char* toString(PumpState s) {
  switch (s) {
    case PumpState::UNKNOWN:         return "UNKNOWN";
    case PumpState::IDLE:            return "IDLE";
    case PumpState::CHOKING:         return "CHOKING";
    case PumpState::CRANKING:        return "CRANKING";
    case PumpState::UNCHOKING:       return "UNCHOKING";
    case PumpState::RUNNING_ASSUMED: return "RUNNING_ASSUMED";
    case PumpState::STOPPING:        return "STOPPING";
    case PumpState::RETRY_WAIT:      return "RETRY_WAIT";
    case PumpState::FAULT:           return "FAULT";
  }
  return "UNKNOWN";
}

const char* toString(CommandType c) {
  switch (c) {
    case CommandType::NONE:         return "NONE";
    case CommandType::START:        return "START";
    case CommandType::STOP:         return "STOP";
    case CommandType::START_FAILED: return "START_FAILED";
    case CommandType::RESET_IDLE:   return "RESET_IDLE";
  }
  return "NONE";
}

const char* toString(EngineStatus e) {
  switch (e) {
    case EngineStatus::UNKNOWN_STATUS:         return "UNKNOWN";
    case EngineStatus::STOPPED_ASSUMED:        return "STOPPED_ASSUMED";
    case EngineStatus::STARTING:               return "STARTING";
    case EngineStatus::RUNNING_ASSUMED_STATUS: return "RUNNING_ASSUMED";
    case EngineStatus::STOPPING_STATUS:        return "STOPPING";
  }
  return "UNKNOWN";
}

const char* toString(FaultCode f) {
  switch (f) {
    case FaultCode::NONE:                  return nullptr;
    case FaultCode::STARTER_KILL_CONFLICT: return "STARTER_KILL_CONFLICT";
    case FaultCode::STARTER_OVERRUN:       return "STARTER_OVERRUN";
    case FaultCode::CHOKE_OVERRUN:         return "CHOKE_OVERRUN";
    case FaultCode::SPARE_ACTIVE:          return "SPARE_ACTIVE";
    case FaultCode::ILLEGAL_TRANSITION:    return "ILLEGAL_TRANSITION";
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Relay layer — the only place digitalWrite() is called
// ---------------------------------------------------------------------------

void PumpController::setRelayOutput(uint8_t pin, bool active) {
  // Polarity is applied exactly once, here.
  const uint8_t level = RELAY_ACTIVE_LOW ? (active ? LOW : HIGH)
                                         : (active ? HIGH : LOW);
  digitalWrite(pin, level);
}

void PumpController::initRelayPin(uint8_t pin) {
  // Drive the inactive level into the output data register BEFORE the pin
  // is switched to an output, so the pin never presents the active level.
  // The redundant post-pinMode write costs nothing and removes any doubt.
  setRelayOutput(pin, false);
  pinMode(pin, OUTPUT);
  setRelayOutput(pin, false);
}

void PumpController::setStarterRelay(bool active) {
  if (active) {
    // Hard interlock: the starter may never be engaged while the ignition
    // kill circuit is grounded. This is checked here, at the lowest layer,
    // not only in the API handlers.
    if (killActive_) {
      Serial.println(F("[SAFETY] refused starter: kill relay active"));
      enterFault(FaultCode::STARTER_KILL_CONFLICT);
      return;
    }
    if (!starterActive_) {
      starterActive_ = true;
      starterOnAt_ = now_;
      setRelayOutput(PIN_RELAY_STARTER, true);
      Serial.println(F("[RELAY] starter ON"));
    }
  } else {
    if (starterActive_) {
      starterActive_ = false;
      setRelayOutput(PIN_RELAY_STARTER, false);
      noteStarterReleased();
      Serial.println(F("[RELAY] starter OFF"));
    } else {
      // Idempotent: keep the physical line asserted inactive regardless.
      setRelayOutput(PIN_RELAY_STARTER, false);
    }
  }
}

void PumpController::setChokeRelay(bool active) {
  if (active) {
    if (!chokeActive_) {
      chokeActive_ = true;
      chokeOnAt_ = now_;
      setRelayOutput(PIN_RELAY_CHOKE, true);
      Serial.println(F("[RELAY] choke ON"));
    }
  } else {
    if (chokeActive_) {
      chokeActive_ = false;
      setRelayOutput(PIN_RELAY_CHOKE, false);
      Serial.println(F("[RELAY] choke OFF"));
    } else {
      setRelayOutput(PIN_RELAY_CHOKE, false);
    }
  }
}

void PumpController::setKillRelay(bool active) {
  if (active) {
    // Hard interlock: grounding the kill circuit always releases the starter
    // first. Order matters electrically and is enforced here.
    if (starterActive_) {
      starterActive_ = false;
      setRelayOutput(PIN_RELAY_STARTER, false);
      noteStarterReleased();
      Serial.println(F("[RELAY] starter OFF (forced by kill)"));
    }
    if (!killActive_) {
      killActive_ = true;
      setRelayOutput(PIN_RELAY_KILL, true);
      Serial.println(F("[RELAY] kill ON"));
    }
  } else {
    if (killActive_) {
      killActive_ = false;
      setRelayOutput(PIN_RELAY_KILL, false);
      Serial.println(F("[RELAY] kill OFF"));
    } else {
      setRelayOutput(PIN_RELAY_KILL, false);
    }
  }
}

void PumpController::setSpareRelay(bool active) {
  // K4 is reserved. It is never activated by this firmware revision; the
  // argument exists only so the call site reads consistently.
  (void)active;
  spareActive_ = false;
  setRelayOutput(PIN_RELAY_SPARE, false);
}

void PumpController::allRelaysInactive() {
  setStarterRelay(false);
  setChokeRelay(false);
  setKillRelay(false);
  setSpareRelay(false);
}

void PumpController::noteStarterReleased() {
  lastStarterReleaseAt_ = now_;
  starterEverReleased_ = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void PumpController::begin(uint32_t now) {
  now_ = now;

  // Every output goes to its inactive electrical level before it is enabled
  // as an output. This is the first hardware action the firmware performs.
  initRelayPin(PIN_RELAY_STARTER);
  initRelayPin(PIN_RELAY_CHOKE);
  initRelayPin(PIN_RELAY_KILL);
  initRelayPin(PIN_RELAY_SPARE);

  starterActive_ = false;
  chokeActive_   = false;
  killActive_    = false;
  spareActive_   = false;

  fault_ = FaultCode::NONE;
  faultKillActive_ = false;
  starterEverReleased_ = false;

  // Never resume an engine operation across a reboot. The controller cannot
  // know whether the engine is running, so it says so.
  state_ = PumpState::UNKNOWN;
  stateEnteredAt_ = now;

  memset(records_, 0, sizeof(records_));
  recordNext_ = 0;
  lastCommandType_ = CommandType::NONE;
  lastCommandRequestId_[0] = '\0';
  lastCommandAccepted_ = false;
}

void PumpController::enterState(PumpState next) {
  if (next == state_) {
    return;
  }
  Serial.print(F("[STATE] "));
  Serial.print(toString(state_));
  Serial.print(F(" -> "));
  Serial.println(toString(next));
  state_ = next;
  stateEnteredAt_ = now_;
}

void PumpController::enterFault(FaultCode code) {
  const bool mightBeRunning = (state_ == PumpState::CRANKING ||
                               state_ == PumpState::UNCHOKING ||
                               state_ == PumpState::RUNNING_ASSUMED ||
                               state_ == PumpState::STOPPING ||
                               state_ == PumpState::UNKNOWN);

  fault_ = code;
  Serial.print(F("[FAULT] "));
  const char* name = toString(code);
  Serial.println(name != nullptr ? name : "NONE");

  // Always shed the two relays that can do harm.
  setStarterRelay(false);
  setChokeRelay(false);
  setSpareRelay(false);

  if (mightBeRunning) {
    // Prefer stop/kill behaviour: if the engine could be turning, ground the
    // ignition-kill circuit for the normal hold time, then release it.
    setKillRelay(true);
    faultKillActive_ = true;
    faultKillStartedAt_ = now_;
  } else {
    setKillRelay(false);
    faultKillActive_ = false;
  }

  enterState(PumpState::FAULT);
}

// ---------------------------------------------------------------------------
// Safety enforcement — runs before the state machine on every tick
// ---------------------------------------------------------------------------

void PumpController::enforceSafety(uint32_t now) {
  now_ = now;

  // 1. Starter and kill must never be commanded active simultaneously.
  if (starterActive_ && killActive_) {
    setRelayOutput(PIN_RELAY_STARTER, false);
    starterActive_ = false;
    noteStarterReleased();
    if (fault_ != FaultCode::STARTER_KILL_CONFLICT) {
      enterFault(FaultCode::STARTER_KILL_CONFLICT);
    }
    return;
  }

  // 2. The spare relay must never be active.
  if (spareActive_) {
    setRelayOutput(PIN_RELAY_SPARE, false);
    spareActive_ = false;
    if (fault_ != FaultCode::SPARE_ACTIVE) {
      enterFault(FaultCode::SPARE_ACTIVE);
    }
    return;
  }

  // 3. Absolute starter engagement ceiling. This backstop is independent of
  //    the state machine: even if advance() stops working, the starter is
  //    released here. Under normal operation CRANK_DURATION_MS expires first.
  if (starterActive_ &&
      static_cast<uint32_t>(now - starterOnAt_) >= MAX_CRANK_MS) {
    setRelayOutput(PIN_RELAY_STARTER, false);
    starterActive_ = false;
    noteStarterReleased();
    Serial.println(F("[SAFETY] starter forced off at MAX_CRANK_MS"));
    enterFault(FaultCode::STARTER_OVERRUN);
    return;
  }

  // 4. The choke must not stay engaged indefinitely after a completed or
  //    aborted start.
  if (chokeActive_ &&
      static_cast<uint32_t>(now - chokeOnAt_) >= MAX_CHOKE_MS) {
    setRelayOutput(PIN_RELAY_CHOKE, false);
    chokeActive_ = false;
    Serial.println(F("[SAFETY] choke forced off at MAX_CHOKE_MS"));
    enterFault(FaultCode::CHOKE_OVERRUN);
    return;
  }
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void PumpController::advance(uint32_t now) {
  now_ = now;
  const uint32_t elapsed = static_cast<uint32_t>(now - stateEnteredAt_);

  switch (state_) {
    case PumpState::UNKNOWN:
    case PumpState::IDLE:
    case PumpState::RUNNING_ASSUMED:
      // Quiescent states. Nothing is timed; all relays stay inactive.
      break;

    case PumpState::CHOKING:
      if (elapsed >= CHOKE_PREP_MS) {
        // Kill must be open before the starter is engaged.
        setKillRelay(false);
        setStarterRelay(true);
        if (starterActive_) {
          enterState(PumpState::CRANKING);
        }
        // If setStarterRelay refused, enterFault() has already run.
      }
      break;

    case PumpState::CRANKING:
      if (elapsed >= CRANK_DURATION_MS) {
        // Releases the starter and records the exact release instant.
        setStarterRelay(false);
        enterState(PumpState::UNCHOKING);
      }
      break;

    case PumpState::UNCHOKING:
      if (elapsed >= UNCHOKE_DELAY_MS) {
        setChokeRelay(false);
        enterState(PumpState::RUNNING_ASSUMED);
      }
      break;

    case PumpState::STOPPING:
      if (elapsed >= KILL_HOLD_MS) {
        setKillRelay(false);
        enterState(PumpState::IDLE);
      }
      break;

    case PumpState::RETRY_WAIT:
      if (cooldownRemainingMs(now) == 0) {
        enterState(PumpState::IDLE);
      }
      break;

    case PumpState::FAULT:
      if (faultKillActive_ &&
          static_cast<uint32_t>(now - faultKillStartedAt_) >= KILL_HOLD_MS) {
        setKillRelay(false);
        faultKillActive_ = false;
        Serial.println(F("[FAULT] kill hold complete; awaiting operator reset"));
      }
      break;
  }
}

void PumpController::tick(uint32_t now) {
  enforceSafety(now);
  advance(now);
}

void PumpController::beginStopSequence() {
  // Order is mandated: starter off, choke off, then kill on.
  setStarterRelay(false);
  setChokeRelay(false);
  setSpareRelay(false);
  setKillRelay(true);
  faultKillActive_ = false;
  fault_ = FaultCode::NONE;
  enterState(PumpState::STOPPING);
  // A STOP entered from STOPPING restarts the hold window, which is harmless
  // and keeps the kill circuit grounded for at least KILL_HOLD_MS more.
  stateEnteredAt_ = now_;
}

// ---------------------------------------------------------------------------
// Observers
// ---------------------------------------------------------------------------

EngineStatus PumpController::engineStatus() const {
  switch (state_) {
    case PumpState::UNKNOWN:         return EngineStatus::UNKNOWN_STATUS;
    case PumpState::FAULT:           return EngineStatus::UNKNOWN_STATUS;
    case PumpState::IDLE:            return EngineStatus::STOPPED_ASSUMED;
    case PumpState::RETRY_WAIT:      return EngineStatus::STOPPED_ASSUMED;
    case PumpState::CHOKING:         return EngineStatus::STOPPED_ASSUMED;
    case PumpState::CRANKING:        return EngineStatus::STARTING;
    case PumpState::UNCHOKING:       return EngineStatus::STARTING;
    case PumpState::RUNNING_ASSUMED: return EngineStatus::RUNNING_ASSUMED_STATUS;
    case PumpState::STOPPING:        return EngineStatus::STOPPING_STATUS;
  }
  return EngineStatus::UNKNOWN_STATUS;
}

uint32_t PumpController::cooldownRemainingMs(uint32_t now) const {
  if (!starterEverReleased_) {
    return 0;
  }
  if (starterActive_) {
    // Still cranking: the full gap has not begun.
    return MIN_RECRANK_GAP_MS;
  }
  const uint32_t since = static_cast<uint32_t>(now - lastStarterReleaseAt_);
  if (since >= MIN_RECRANK_GAP_MS) {
    return 0;
  }
  return MIN_RECRANK_GAP_MS - since;
}

bool PumpController::isQuiescent() const {
  if (starterActive_ || chokeActive_ || killActive_ || spareActive_) {
    return false;
  }
  switch (state_) {
    case PumpState::CHOKING:
    case PumpState::CRANKING:
    case PumpState::UNCHOKING:
    case PumpState::STOPPING:
      return false;
    default:
      return true;
  }
}

bool PumpController::startPermitted(uint32_t now) const {
  return state_ == PumpState::IDLE && cooldownRemainingMs(now) == 0;
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

const PumpController::CommandRecord* PumpController::findRecord(
    CommandType type, const char* requestId) const {
  if (requestId == nullptr || requestId[0] == '\0') {
    return nullptr;
  }
  for (uint8_t i = 0; i < IDEMPOTENCY_SLOTS; ++i) {
    const CommandRecord& r = records_[i];
    if (r.used && r.type == type && strcmp(r.requestId, requestId) == 0) {
      return &r;
    }
  }
  return nullptr;
}

void PumpController::recordCommand(CommandType type, const char* requestId,
                                   bool accepted, uint16_t httpStatus) {
  if (requestId == nullptr || requestId[0] == '\0') {
    return;
  }
  // Only accepted commands are remembered. A rejection is not a committed
  // operation, so replaying its ID later must be re-evaluated on merit.
  if (!accepted) {
    return;
  }
  CommandRecord& r = records_[recordNext_];
  recordNext_ = static_cast<uint8_t>((recordNext_ + 1) % IDEMPOTENCY_SLOTS);

  strncpy(r.requestId, requestId, REQUEST_ID_MAX_LEN);
  r.requestId[REQUEST_ID_MAX_LEN] = '\0';
  r.type = type;
  r.accepted = accepted;
  r.httpStatus = httpStatus;
  r.used = true;
}

void PumpController::rememberLastCommand(CommandType type, const char* requestId,
                                         bool accepted) {
  lastCommandType_ = type;
  lastCommandAccepted_ = accepted;
  if (requestId != nullptr) {
    strncpy(lastCommandRequestId_, requestId, REQUEST_ID_MAX_LEN);
    lastCommandRequestId_[REQUEST_ID_MAX_LEN] = '\0';
  } else {
    lastCommandRequestId_[0] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Command entry point
// ---------------------------------------------------------------------------

CommandResult PumpController::handleCommand(CommandType type,
                                            const char* requestId,
                                            uint32_t now) {
  now_ = now;

  // Safety limits are re-evaluated before any command is acted on, so a
  // command can never be serviced on top of a stale, unsafe relay picture.
  enforceSafety(now);

  CommandResult out{};
  out.duplicate = false;

  const CommandRecord* prior = findRecord(type, requestId);
  if (prior != nullptr) {
    // Same request ID and same command: do not execute again.
    Serial.print(F("[CMD] duplicate ignored: "));
    Serial.println(toString(type));
    out.accepted = prior->accepted;
    out.duplicate = true;
    out.httpStatus = prior->httpStatus;
    out.state = state_;
    out.cooldownRemainingMs = cooldownRemainingMs(now);
    return out;
  }

  Serial.print(F("[CMD] "));
  Serial.print(toString(type));
  Serial.print(F(" in state "));
  Serial.println(toString(state_));

  switch (type) {
    case CommandType::STOP: {
      // STOP has priority over every other operation and is accepted from
      // every state without exception.
      beginStopSequence();
      out.accepted = true;
      out.httpStatus = 202;
      break;
    }

    case CommandType::START: {
      if (!startPermitted(now)) {
        out.accepted = false;
        out.httpStatus = 409;
        break;
      }
      // 1. kill open, 2. starter inactive, 3. choke on, 4. CHOKING.
      setKillRelay(false);
      setStarterRelay(false);
      setChokeRelay(true);
      enterState(PumpState::CHOKING);
      out.accepted = true;
      out.httpStatus = 202;
      break;
    }

    case CommandType::START_FAILED: {
      const bool allowed = (state_ == PumpState::RUNNING_ASSUMED ||
                            state_ == PumpState::RETRY_WAIT ||
                            state_ == PumpState::IDLE ||
                            state_ == PumpState::UNKNOWN);
      if (!allowed) {
        out.accepted = false;
        out.httpStatus = 409;
        break;
      }
      setStarterRelay(false);
      setChokeRelay(false);
      setKillRelay(false);
      setSpareRelay(false);
      // RETRY_WAIT holds until the minimum starter interval has elapsed and
      // then falls back to IDLE. No automatic retry is ever initiated.
      enterState(PumpState::RETRY_WAIT);
      out.accepted = true;
      out.httpStatus = 202;
      break;
    }

    case CommandType::RESET_IDLE: {
      const bool allowed = (state_ == PumpState::UNKNOWN ||
                            state_ == PumpState::IDLE ||
                            state_ == PumpState::RETRY_WAIT ||
                            state_ == PumpState::FAULT);
      if (!allowed) {
        out.accepted = false;
        out.httpStatus = 409;
        break;
      }
      allRelaysInactive();
      fault_ = FaultCode::NONE;
      faultKillActive_ = false;
      // The operator asserts the engine is stopped; the controller still
      // honours any outstanding starter-protection cooldown.
      if (cooldownRemainingMs(now) > 0) {
        enterState(PumpState::RETRY_WAIT);
      } else {
        enterState(PumpState::IDLE);
      }
      out.accepted = true;
      out.httpStatus = 202;
      break;
    }

    case CommandType::NONE:
    default:
      out.accepted = false;
      out.httpStatus = 400;
      break;
  }

  out.state = state_;
  out.cooldownRemainingMs = cooldownRemainingMs(now);

  recordCommand(type, requestId, out.accepted, out.httpStatus);
  rememberLastCommand(type, requestId, out.accepted);

  Serial.print(F("[CMD] result accepted="));
  Serial.print(out.accepted ? 1 : 0);
  Serial.print(F(" state="));
  Serial.println(toString(state_));

  return out;
}
