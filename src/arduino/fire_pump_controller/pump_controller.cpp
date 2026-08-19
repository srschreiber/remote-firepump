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
    case PumpState::PRIMING:         return "PRIMING";
    case PumpState::CHOKING:         return "CHOKING";
    case PumpState::CRANKING:        return "CRANKING";
    case PumpState::UNCHOKING:       return "UNCHOKING";
    case PumpState::RUNNING_ASSUMED: return "RUNNING_ASSUMED";
    case PumpState::STOPPING:        return "STOPPING";
    case PumpState::VALVE_CLOSING:   return "VALVE_CLOSING";
    case PumpState::RETRY_WAIT:      return "RETRY_WAIT";
    case PumpState::FAULT:           return "FAULT";
  }
  return "UNKNOWN";
}

const char* toString(CommandType c) {
  switch (c) {
    case CommandType::NONE:              return "NONE";
    case CommandType::START:             return "START";
    case CommandType::STOP:              return "STOP";
    case CommandType::START_FAILED:      return "START_FAILED";
    case CommandType::RESET_IDLE:        return "RESET_IDLE";
    case CommandType::MAINT_CHOKE_ON:    return "MAINT_CHOKE_ON";
    case CommandType::MAINT_CHOKE_OFF:   return "MAINT_CHOKE_OFF";
    case CommandType::MAINT_STARTER_ON:  return "MAINT_STARTER_ON";
    case CommandType::MAINT_STARTER_OFF: return "MAINT_STARTER_OFF";
    case CommandType::MAINT_KILL_ON:     return "MAINT_KILL_ON";
    case CommandType::MAINT_KILL_OFF:    return "MAINT_KILL_OFF";
    case CommandType::MAINT_VALVE_ON:    return "MAINT_VALVE_ON";
    case CommandType::MAINT_VALVE_OFF:   return "MAINT_VALVE_OFF";
  }
  return "NONE";
}

bool isMaintenanceCommand(CommandType c) {
  switch (c) {
    case CommandType::MAINT_CHOKE_ON:
    case CommandType::MAINT_CHOKE_OFF:
    case CommandType::MAINT_STARTER_ON:
    case CommandType::MAINT_STARTER_OFF:
    case CommandType::MAINT_KILL_ON:
    case CommandType::MAINT_KILL_OFF:
    case CommandType::MAINT_VALVE_ON:
    case CommandType::MAINT_VALVE_OFF:
      return true;
    default:
      return false;
  }
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
    case FaultCode::VALVE_CLOSED_WHILE_RUNNING:
      return "VALVE_CLOSED_WHILE_RUNNING";
    case FaultCode::WATER_LOST:            return "WATER_LOST";
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

bool PumpController::engineMayBeRunning() const {
  // Used to REFUSE shutting the intake. Deliberately includes STOPPING: the
  // kill has only just been grounded and the engine is still winding down.
  switch (state_) {
    case PumpState::CRANKING:
    case PumpState::UNCHOKING:
    case PumpState::RUNNING_ASSUMED:
    case PumpState::STOPPING:
      return true;
    default:
      return false;
  }
}

bool PumpController::engineWasStarted() const {
  // Used to detect a pump running DRY. Narrower than engineMayBeRunning():
  // these are the only states reachable by actually cranking, so a shut
  // intake here means the engine really is turning without water. STOPPING is
  // excluded because it is reachable from IDLE, where the engine was never
  // started and a shut intake is entirely correct.
  switch (state_) {
    case PumpState::CRANKING:
    case PumpState::UNCHOKING:
    case PumpState::RUNNING_ASSUMED:
      return true;
    default:
      return false;
  }
}

bool PumpController::primeComplete() const {
  // Water must be present regardless of how the intake is controlled -- that
  // is the actual precondition for cranking, not the position of a valve.
  if (!waterOk_) {
    return false;
  }
  if (!INTAKE_VALVE_ENABLED) {
    // No electric valve in this build: a permanently open manual intake plus
    // a foot/check valve holds prime, so the water interlock is the whole
    // condition.
    return true;
  }
  return valveActive_ &&
         static_cast<uint32_t>(now_ - valveOpenedAt_) >= VALVE_PRIME_MS;
}

void PumpController::setValveRelay(bool active) {
  if (active) {
    if (!valveActive_) {
      valveActive_ = true;
      valveOpenedAt_ = now_;
      setRelayOutput(PIN_RELAY_VALVE, true);
      Serial.println(F("[RELAY] valve OPEN"));
    }
    return;
  }

  // Hard interlock: never shut the INTAKE while the engine could still be
  // turning. Closing it starves the pump of water and it runs dry; the water
  // is what lubricates and cools the mechanical seal, and Honda warn that
  // extended dry running destroys it. The stop sequence therefore always
  // grounds the kill wire first and only shuts the intake after
  // VALVE_CLOSE_DELAY_MS in VALVE_CLOSING.
  //
  // NOTE: this is the software layer only. It cannot help against a broken
  // valve wire, a blown valve fuse or a mechanically stuck valve -- that is
  // what the hardwired water interlock is for. See config.h.
  if (engineMayBeRunning()) {
    Serial.println(F("[SAFETY] refused intake close: engine may be running"));
    return;
  }

  if (valveActive_) {
    valveActive_ = false;
    setRelayOutput(PIN_RELAY_VALVE, false);
    Serial.println(F("[RELAY] valve CLOSED"));
  } else {
    setRelayOutput(PIN_RELAY_VALVE, false);
  }
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
    // Hard interlock: never crank unless priming is complete. "Complete"
    // means the valve is open AND has been open for the full prime dwell --
    // an open valve alone does not mean the pump is primed.
    if (!primeComplete()) {
      Serial.println(F("[SAFETY] refused starter: not primed"));
      enterFault(FaultCode::VALVE_CLOSED_WHILE_RUNNING);
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

void PumpController::driveKillOutput(bool killAsserted) {
  // The ONLY place the fail-safe wiring inversion is applied.
  //
  // With K3 on its NC contact the relay must be ENERGISED for the engine to
  // be permitted to run, so asserting kill means DE-energising the relay.
  // Everything above this line reasons in terms of "kill asserted".
  const bool energise = KILL_RELAY_FAIL_SAFE_NC ? !killAsserted : killAsserted;
  setRelayOutput(PIN_RELAY_KILL, energise);
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
      driveKillOutput(true);
      Serial.println(F("[RELAY] kill ON"));
    }
  } else {
    if (killActive_) {
      killActive_ = false;
      driveKillOutput(false);
      Serial.println(F("[RELAY] kill OFF"));
    } else {
      driveKillOutput(false);
    }
  }
}

void PumpController::toSafeState() {
  // "Safe" is NOT "all relays de-energised". With the fail-safe wiring the
  // kill must be ASSERTED (K3 de-energised, NC contact grounding the ignition)
  // so the engine is inhibited. Releasing it here would leave a magneto engine
  // free to run.
  setStarterRelay(false);
  setChokeRelay(false);
  setKillRelay(true);
  // Refused, and correctly so, if the engine may still be running.
  setValveRelay(false);
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
  initRelayPin(PIN_RELAY_VALVE);

  // Water interlock input. INPUT_PULLUP means a broken wire or absent sensor
  // reads HIGH, which WATER_OK_LEVEL defines as "no water" -- so a wiring
  // failure fails safe.
  pinMode(PIN_WATER_OK, INPUT_PULLUP);

  starterActive_ = false;
  chokeActive_   = false;
  // Fail-safe wiring: initRelayPin() left K3 de-energised, which closes its NC
  // contact and grounds the kill wire. So at boot the kill is ASSERTED -- the
  // engine is inhibited until the controller deliberately permits it to run.
  killActive_    = KILL_RELAY_FAIL_SAFE_NC;
  // Normally-closed valve: de-energised at reset means physically shut.
  valveActive_   = false;

  waterOk_ = false;
  waterRawLast_ = false;
  waterStableSince_ = now;

  fault_ = FaultCode::NONE;
  faultKillActive_ = false;
  starterEverReleased_ = false;
  valveOpenedAt_ = now;

  // Per-request overrides never survive a reboot.
  timings_ = StartTimings();

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

  // Always shed the two relays that can do harm. The valve is deliberately
  // NOT touched here: if the engine might be running, shutting it would
  // deadhead the pump, and if it is not running an open valve is harmless.
  setStarterRelay(false);
  setChokeRelay(false);

  if (mightBeRunning) {
    // Prefer stop/kill behaviour: if the engine could be turning, ground the
    // ignition-kill circuit for the normal hold time, then release it.
    setKillRelay(true);
    faultKillActive_ = true;
    faultKillStartedAt_ = now_;
  } else {
    // Engine is not believed to be running, but assert the kill anyway --
    // it is the resting, fail-safe position.
    setKillRelay(true);
    faultKillActive_ = false;
  }

  enterState(PumpState::FAULT);
}

// ---------------------------------------------------------------------------
// Safety enforcement — runs before the state machine on every tick
// ---------------------------------------------------------------------------

void PumpController::updateWaterInterlock(uint32_t now) {
  if (!WATER_INTERLOCK_REQUIRED) {
    // Bench builds with no sensor fitted. Compile-time-refused whenever the
    // electric intake valve is enabled; see the static_assert in config.h.
    waterOk_ = true;
    return;
  }

  const bool raw = (digitalRead(PIN_WATER_OK) == WATER_OK_LEVEL);
  if (raw != waterRawLast_) {
    waterRawLast_ = raw;
    waterStableSince_ = now;
    return;
  }
  // Only believe a reading that has held steady. Pressure switches chatter
  // around their setpoint and a fire pump must not trip on a momentary dip.
  if (static_cast<uint32_t>(now - waterStableSince_) >= WATER_DEBOUNCE_MS) {
    if (waterOk_ != raw) {
      Serial.print(F("[WATER] "));
      Serial.println(raw ? F("available") : F("LOST"));
    }
    waterOk_ = raw;
  }
}

bool PumpController::waterStartupGraceActive(uint32_t now) const {
  // Straight after a crank the pump has not built pressure yet, so the switch
  // legitimately reads dry for a while.
  return starterEverReleased_ &&
         static_cast<uint32_t>(now - lastStarterReleaseAt_) < WATER_STARTUP_GRACE_MS;
}

void PumpController::enforceSafety(uint32_t now) {
  now_ = now;

  updateWaterInterlock(now);

  // 0. Loss of water while the engine may be running. The hardwired interlock
  //    is the real protection -- it grounds the kill wire without any help
  //    from this firmware -- but shutting down deliberately here means the
  //    controller's reported state matches reality and the intake is closed
  //    in an orderly way afterwards.
  if (engineWasStarted() && !waterOk_ && !waterStartupGraceActive(now) &&
      fault_ != FaultCode::WATER_LOST) {
    Serial.println(F("[SAFETY] water lost while running; stopping engine"));
    enterFault(FaultCode::WATER_LOST);
    return;
  }

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

  // 2. The starter must never be engaged with the discharge valve shut, and
  //    the valve must never be shut while the engine could be turning. Both
  //    directions of the deadhead interlock are re-checked every tick.
  if (starterActive_ && !valveActive_) {
    setRelayOutput(PIN_RELAY_STARTER, false);
    starterActive_ = false;
    noteStarterReleased();
    Serial.println(F("[SAFETY] starter forced off: valve is closed"));
    if (fault_ != FaultCode::VALVE_CLOSED_WHILE_RUNNING) {
      enterFault(FaultCode::VALVE_CLOSED_WHILE_RUNNING);
    }
    return;
  }
  // Narrower than engineMayBeRunning(): STOPPING is reachable from IDLE,
  // where the engine was never started and a shut intake is correct. Only the
  // states we can only have arrived at by cranking imply the engine is
  // actually turning.
  if (engineWasStarted() && !valveActive_) {
    // The engine is turning with the intake shut, so the pump is running dry.
    // Reopening is the only safe response.
    Serial.println(F("[SAFETY] intake reopened: engine is running dry"));
    setValveRelay(true);
    if (fault_ != FaultCode::VALVE_CLOSED_WHILE_RUNNING) {
      enterFault(FaultCode::VALVE_CLOSED_WHILE_RUNNING);
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
      // Quiescent. Nothing is timed; all relays stay inactive.
      break;

    case PumpState::RUNNING_ASSUMED:
      // Quiescent for timing purposes, but the valve stays OPEN for as long
      // as the engine is assumed to be running.
      break;

    case PumpState::PRIMING:
      if (elapsed >= VALVE_PRIME_MS) {
        // Valve has been open long enough to prime; begin the start sequence.
        setChokeRelay(true);
        enterState(PumpState::CHOKING);
      }
      break;

    case PumpState::VALVE_CLOSING:
      if (elapsed >= VALVE_CLOSE_DELAY_MS) {
        // The engine has had time to come to rest, so the intake can now be
        // shut without starving a spinning pump of water.
        setValveRelay(false);
        enterState(PumpState::IDLE);
      }
      break;

    case PumpState::CHOKING:
      if (elapsed >= timings_.chokePrepMs) {
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
      if (elapsed >= timings_.crankMs) {
        // Releases the starter and records the exact release instant.
        setStarterRelay(false);
        enterState(PumpState::UNCHOKING);
      }
      break;

    case PumpState::UNCHOKING:
      if (elapsed >= timings_.unchokeDelayMs) {
        setChokeRelay(false);
        enterState(PumpState::RUNNING_ASSUMED);
      }
      break;

    case PumpState::STOPPING:
      if (elapsed >= KILL_HOLD_MS) {
        // The kill stays ASSERTED. With the fail-safe NC wiring that means K3
        // stays de-energised and the kill wire stays grounded, which is the
        // resting state for a stopped engine -- releasing it here would let
        // the engine be pull-started or restarted unattended.
        //
        // Leaving STOPPING clears engineMayBeRunning(), which is what permits
        // the intake to be shut once VALVE_CLOSE_DELAY_MS has also elapsed.
        enterState(PumpState::VALVE_CLOSING);
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
      // The valve is intentionally left as it is. Closing it is an operator
      // decision made via /v1/stop or /v1/reset-idle, once they know the
      // engine really has stopped.
      break;
  }
}

void PumpController::tick(uint32_t now) {
  enforceSafety(now);
  advance(now);
}

void PumpController::beginStopSequence() {
  // Order is mandated: starter off, choke off, then kill on.
  //
  // The valve is deliberately left OPEN here. It is closed only in
  // VALVE_CLOSING, after the kill hold has expired and the engine has been
  // given VALVE_CLOSE_DELAY_MS to come to rest -- never against a pump that
  // could still be spinning.
  setStarterRelay(false);
  setChokeRelay(false);
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
    case PumpState::PRIMING:         return EngineStatus::STOPPED_ASSUMED;
    case PumpState::CHOKING:         return EngineStatus::STOPPED_ASSUMED;
    case PumpState::CRANKING:        return EngineStatus::STARTING;
    case PumpState::UNCHOKING:       return EngineStatus::STARTING;
    case PumpState::RUNNING_ASSUMED: return EngineStatus::RUNNING_ASSUMED_STATUS;
    case PumpState::STOPPING:        return EngineStatus::STOPPING_STATUS;
    case PumpState::VALVE_CLOSING:   return EngineStatus::STOPPING_STATUS;
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
  // Quiescence means "no relay TIMING is pending", which is the only thing
  // that makes a blocking Wi-Fi call dangerous.
  //
  // The kill and the valve are both deliberately excluded. With the fail-safe
  // wiring the kill is ASSERTED at rest, so treating it as activity would
  // mark IDLE as busy forever; and the valve stays open for the whole time
  // the engine runs, which could be hours. Neither has a timing sequence of
  // its own -- the states below are what carry those.
  if (starterActive_ || chokeActive_) {
    return false;
  }
  switch (state_) {
    case PumpState::PRIMING:
    case PumpState::CHOKING:
    case PumpState::CRANKING:
    case PumpState::UNCHOKING:
    case PumpState::STOPPING:
    case PumpState::VALVE_CLOSING:
      return false;
    default:
      return true;
  }
}

bool PumpController::startPermitted(uint32_t now) const {
  if (state_ != PumpState::IDLE || cooldownRemainingMs(now) != 0) {
    return false;
  }
  // Water must be available before the engine is allowed to run at all.
  if (!waterOk_) {
    return false;
  }
  // The starter and choke must be at rest, and the kill must currently be
  // asserted -- IDLE means "engine inhibited", so a released kill here would
  // mean something has already permitted the engine to run.
  if (starterActive_ || chokeActive_ || !killActive_) {
    return false;
  }
  // The intake must be shut so the prime dwell is genuinely observed rather
  // than inherited from an earlier run or a manual maintenance command.
  if (INTAKE_VALVE_ENABLED && valveActive_) {
    return false;
  }
  return true;
}

bool PumpController::maintenancePermitted() const {
  // Only from a settled state. Never mid-sequence, never from FAULT.
  return state_ == PumpState::IDLE || state_ == PumpState::UNKNOWN;
}

bool PumpController::applyMaintenance(CommandType type) {
  switch (type) {
    case CommandType::MAINT_CHOKE_ON:
      setChokeRelay(true);
      return chokeActive_;

    case CommandType::MAINT_CHOKE_OFF:
      setChokeRelay(false);
      return true;

    case CommandType::MAINT_STARTER_ON:
      // The relay layer refuses and faults if kill is grounded; the caller
      // is expected to have checked, but this is the authoritative gate.
      if (killActive_) {
        return false;
      }
      setStarterRelay(true);
      return starterActive_;

    case CommandType::MAINT_STARTER_OFF:
      setStarterRelay(false);
      return true;

    case CommandType::MAINT_KILL_ON:
      // setKillRelay() releases the starter first, in that order.
      setKillRelay(true);
      return killActive_;

    case CommandType::MAINT_KILL_OFF:
      setKillRelay(false);
      return true;

    case CommandType::MAINT_VALVE_ON:
      setValveRelay(true);
      return valveActive_;

    case CommandType::MAINT_VALVE_OFF:
      // Refused by the relay layer if the engine may be running. Maintenance
      // is only permitted from IDLE/UNKNOWN anyway, so this should always
      // succeed -- the check below is what makes that a guarantee, not a
      // convention.
      setValveRelay(false);
      return !valveActive_;

    default:
      return false;
  }
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
                                            uint32_t now,
                                            const StartTimings* timings) {
  now_ = now;

  // Safety limits are re-evaluated before any command is acted on, so a
  // command can never be serviced on top of a stale, unsafe relay picture.
  enforceSafety(now);

  CommandResult out{};
  out.duplicate = false;

  const CommandRecord* prior = findRecord(type, requestId);
  const bool isDuplicate = (prior != nullptr);

  // Duplicate suppression exists to stop a retried network request from
  // cranking the engine twice. It deliberately does NOT apply to STOP.
  //
  // Failing to execute a stop is dangerous; executing one twice is not -- it
  // simply re-grounds the ignition-kill circuit. Suppressing a replayed STOP
  // would mean a stale retransmission arriving after a later START could be
  // silently swallowed while the engine is cranking. STOP therefore always
  // runs, and is only *labelled* as a duplicate in the response.
  if (isDuplicate && type != CommandType::STOP) {
    Serial.print(F("[CMD] duplicate ignored: "));
    Serial.println(toString(type));
    out.accepted = prior->accepted;
    out.duplicate = true;
    out.httpStatus = prior->httpStatus;
    out.state = state_;
    out.cooldownRemainingMs = cooldownRemainingMs(now);
    return out;
  }
  out.duplicate = isDuplicate;

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
      // Latch the timings this run will use. clamp() is applied here as well
      // as at the HTTP layer, so no caller can request a longer crank than
      // the hard ceiling even if validation is ever bypassed.
      timings_ = (timings != nullptr) ? *timings : StartTimings();
      timings_.clamp();

      // 1. kill open, 2. starter inactive, 3. valve OPEN to prime.
      //
      // The choke and starter come later: PRIMING holds the valve open for
      // VALVE_PRIME_MS before advance() moves on to CHOKING. The starter is
      // additionally gated on primeComplete() at the relay layer, so no path
      // can crank an unprimed pump.
      //
      // The kill stays ASSERTED throughout priming. The engine is only
      // permitted to run at the moment the starter is engaged, in advance().
      setStarterRelay(false);
      if (INTAKE_VALVE_ENABLED) {
        setValveRelay(true);
        enterState(PumpState::PRIMING);
      } else {
        // No electric intake valve: nothing to prime, so go straight to the
        // choke. primeComplete() still gates the starter on the water
        // interlock.
        setChokeRelay(true);
        enterState(PumpState::CHOKING);
      }
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
      // Assert the kill: the engine did not catch, and a magneto engine is
      // only inhibited while K3 is de-energised.
      setKillRelay(true);
      // The engine did not catch, so there is nothing to deadhead: shut the
      // valve. A later retry therefore re-primes from scratch rather than
      // inheriting a stale prime.
      //
      // enterState() first so engineMayBeRunning() is already false; the
      // relay layer would otherwise refuse the close from RUNNING_ASSUMED.
      enterState(PumpState::RETRY_WAIT);
      setValveRelay(false);
      // RETRY_WAIT holds until the minimum starter interval has elapsed and
      // then falls back to IDLE. No automatic retry is ever initiated.
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
      fault_ = FaultCode::NONE;
      faultKillActive_ = false;
      // The operator asserts the engine is stopped; the controller still
      // honours any outstanding starter-protection cooldown.
      if (cooldownRemainingMs(now) > 0) {
        enterState(PumpState::RETRY_WAIT);
      } else {
        enterState(PumpState::IDLE);
      }
      // After the state change, so engineMayBeRunning() is false and the
      // valve close is permitted. The operator has asserted the engine is
      // stopped, which is exactly the assertion the interlock needs.
      toSafeState();
      out.accepted = true;
      out.httpStatus = 202;
      break;
    }

    case CommandType::MAINT_CHOKE_ON:
    case CommandType::MAINT_CHOKE_OFF:
    case CommandType::MAINT_STARTER_ON:
    case CommandType::MAINT_STARTER_OFF:
    case CommandType::MAINT_KILL_ON:
    case CommandType::MAINT_KILL_OFF:
    case CommandType::MAINT_VALVE_ON:
    case CommandType::MAINT_VALVE_OFF: {
      if (!maintenancePermitted()) {
        out.accepted = false;
        out.httpStatus = 409;
        break;
      }
      if (!applyMaintenance(type)) {
        // Refused by an interlock -- most commonly starter-on while the kill
        // circuit is grounded.
        out.accepted = false;
        out.httpStatus = 409;
        break;
      }
      // Deliberately no state change: manual relay work happens in place.
      // startPermitted() already refuses while any relay is energised, so a
      // START cannot be layered on top of a manually driven relay.
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

  // A re-executed STOP must not consume another ring slot, or a single
  // retried ID could evict the whole idempotency history.
  if (!isDuplicate) {
    recordCommand(type, requestId, out.accepted, out.httpStatus);
  }
  rememberLastCommand(type, requestId, out.accepted);

  Serial.print(F("[CMD] result accepted="));
  Serial.print(out.accepted ? 1 : 0);
  Serial.print(F(" state="));
  Serial.println(toString(state_));

  return out;
}
