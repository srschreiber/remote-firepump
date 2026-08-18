// serial_console.cpp — see serial_console.h.

#include "serial_console.h"

#if ENABLE_SERIAL_CONSOLE

#include <string.h>

#include "net_manager.h"

namespace {

// Case-insensitive whole-token compare.
bool tokenIs(const char* tok, const char* literal) {
  size_t i = 0;
  for (; tok[i] != '\0' && literal[i] != '\0'; ++i) {
    char a = tok[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (a != literal[i]) {
      return false;
    }
  }
  return tok[i] == '\0' && literal[i] == '\0';
}

}  // namespace

void SerialConsole::begin() {
  Serial.println();
  Serial.println(F("=== bench console ENABLED (ENABLE_SERIAL_CONSOLE=1) ==="));
  Serial.println(F("This drives real relays. Disconnect the 12 V contact side."));
  Serial.println(F("Type 'help' for commands, 'test' for a relay lamp test."));
  Serial.println();
}

void SerialConsole::printHelp() const {
  Serial.println(F("commands:"));
  Serial.println(F("  help              this list"));
  Serial.println(F("  status            state, relays, wifi, fault"));
  Serial.println(F("  test              LAMP TEST: pulse K1, K2, K3 in turn"));
  Serial.println(F("  scan              list visible 2.4 GHz networks"));
  Serial.println(F("  choke   on|off    drive K2 (D3/IN2)"));
  Serial.println(F("  starter on|off    drive K1 (D2/IN1)"));
  Serial.println(F("  kill    on|off    drive K3 (D4/IN3)"));
  Serial.println(F("  start             run the full start sequence"));
  Serial.println(F("  stop              stop now, from any state"));
  Serial.println(F("  failed            report that the start did not work"));
  Serial.println(F("  reset             confirm engine stopped -> IDLE"));
}

void SerialConsole::printStatus(const PumpController& pump,
                                const NetManager& net) const {
  const uint32_t now = millis();
  Serial.print(F("state="));
  Serial.print(toString(pump.state()));
  Serial.print(F(" engine="));
  Serial.print(toString(pump.engineStatus()));
  Serial.print(F(" running_confirmed=false"));
  Serial.println();

  Serial.print(F("relays: starter="));
  Serial.print(pump.starterActive() ? "ON " : "off");
  Serial.print(F(" choke="));
  Serial.print(pump.chokeActive() ? "ON " : "off");
  Serial.print(F(" kill="));
  Serial.print(pump.killActive() ? "ON " : "off");
  Serial.print(F(" spare="));
  Serial.println(pump.spareActive() ? "ON " : "off");

  Serial.print(F("pin levels (active="));
  Serial.print(RELAY_ACTIVE_LOW ? F("LOW") : F("HIGH"));
  Serial.print(F("): D2="));
  Serial.print(digitalRead(PIN_RELAY_STARTER) ? F("HIGH") : F("LOW"));
  Serial.print(F(" D3="));
  Serial.print(digitalRead(PIN_RELAY_CHOKE) ? F("HIGH") : F("LOW"));
  Serial.print(F(" D4="));
  Serial.print(digitalRead(PIN_RELAY_KILL) ? F("HIGH") : F("LOW"));
  Serial.print(F(" D5="));
  Serial.println(digitalRead(PIN_RELAY_SPARE) ? F("HIGH") : F("LOW"));

  Serial.print(F("cooldown_ms="));
  Serial.print(pump.cooldownRemainingMs(now));
  Serial.print(F(" state_elapsed_ms="));
  Serial.println(pump.stateElapsedMs(now));

  Serial.print(F("wifi="));
  Serial.print(net.isConnected() ? F("connected") : F("down"));
  Serial.print(F(" ip="));
  Serial.print(net.ipString());
  Serial.print(F(" mac="));
  Serial.print(net.macString());
  Serial.print(F(" rssi="));
  Serial.println(net.rssiDbm());

  const char* fault = toString(pump.fault());
  Serial.print(F("fault="));
  Serial.println(fault != nullptr ? fault : "none");
}

void SerialConsole::runCommand(CommandType type, const char* label,
                               PumpController& pump, uint32_t now) {
  // A fresh request ID each time: console commands are explicit operator
  // actions, never network retries, so they must never be deduplicated.
  char id[REQUEST_ID_MAX_LEN + 1];
  snprintf(id, sizeof(id), "console-%s-%lu", label,
           static_cast<unsigned long>(now));

  const CommandResult r = pump.handleCommand(type, id, now);
  Serial.print(r.accepted ? F("-> accepted, state=") : F("-> REFUSED, state="));
  Serial.print(toString(r.state));
  if (!r.accepted) {
    Serial.print(F(" (http "));
    Serial.print(r.httpStatus);
    Serial.print(F(", cooldown "));
    Serial.print(r.cooldownRemainingMs);
    Serial.print(F(" ms)"));
  }
  Serial.println();
}

// ---------------------------------------------------------------------------
// Lamp test
// ---------------------------------------------------------------------------

void SerialConsole::startLampTest(uint32_t now, PumpController& pump) {
  if (lamp_ != LampPhase::OFF) {
    Serial.println(F("-> lamp test already running"));
    return;
  }
  if (pump.state() != PumpState::IDLE && pump.state() != PumpState::UNKNOWN) {
    Serial.print(F("-> REFUSED: lamp test needs IDLE or UNKNOWN, state="));
    Serial.println(toString(pump.state()));
    return;
  }

  Serial.println(F("lamp test: K2 choke -> K1 starter -> K3 kill"));
  Serial.println(F("watch IN2, IN1, IN3 in that order; 'stop' aborts"));
  lamp_ = LampPhase::CHOKE_ON;
  lampPhaseAt_ = now;
  // Fire the first pulse here rather than from advanceLampTest(). Driving it
  // from an `elapsed == 0` branch there re-fired on every loop iteration that
  // landed in the same millisecond.
  fireLamp(CommandType::MAINT_CHOKE_ON, "K2 choke   (D3/IN2) ON ", pump, now);
}

void SerialConsole::fireLamp(CommandType type, const char* what,
                             PumpController& pump, uint32_t now) {
  // Unique ID per pulse so nothing is suppressed as a duplicate.
  char id[REQUEST_ID_MAX_LEN + 1];
  snprintf(id, sizeof(id), "lamp-%u", static_cast<unsigned>(lampSeq_++));
  const CommandResult r = pump.handleCommand(type, id, now);
  Serial.print(F("  "));
  Serial.print(what);
  Serial.println(r.accepted ? F(" -> ok") : F(" -> REFUSED"));
}

void SerialConsole::advanceLampTest(uint32_t now, PumpController& pump) {
  if (lamp_ == LampPhase::OFF) {
    return;
  }

  // Abort if anything took the controller somewhere unexpected -- a fault, or
  // a STOP arriving mid-test.
  if (pump.state() != PumpState::IDLE && pump.state() != PumpState::UNKNOWN) {
    Serial.println(F("lamp test: aborted (controller left IDLE/UNKNOWN)"));
    pump.handleCommand(CommandType::MAINT_CHOKE_OFF, "", now);
    pump.handleCommand(CommandType::MAINT_STARTER_OFF, "", now);
    lamp_ = LampPhase::OFF;
    return;
  }

  const uint32_t elapsed = static_cast<uint32_t>(now - lampPhaseAt_);
  const bool pulseDone = elapsed >= LAMP_TEST_PULSE_MS;
  const bool gapDone   = elapsed >= LAMP_TEST_GAP_MS;

  switch (lamp_) {
    case LampPhase::CHOKE_ON:
      if (pulseDone) {
        fireLamp(CommandType::MAINT_CHOKE_OFF, "K2 choke   (D3/IN2) off", pump, now);
        lamp_ = LampPhase::CHOKE_OFF;
        lampPhaseAt_ = now;
      }
      break;

    case LampPhase::CHOKE_OFF:
      if (gapDone) {
        fireLamp(CommandType::MAINT_STARTER_ON, "K1 starter (D2/IN1) ON ", pump, now);
        lamp_ = LampPhase::STARTER_ON;
        lampPhaseAt_ = now;
      }
      break;

    case LampPhase::STARTER_ON:
      if (pulseDone) {
        fireLamp(CommandType::MAINT_STARTER_OFF, "K1 starter (D2/IN1) off", pump, now);
        lamp_ = LampPhase::STARTER_OFF;
        lampPhaseAt_ = now;
      }
      break;

    case LampPhase::STARTER_OFF:
      if (gapDone) {
        fireLamp(CommandType::MAINT_KILL_ON, "K3 kill    (D4/IN3) ON ", pump, now);
        lamp_ = LampPhase::KILL_ON;
        lampPhaseAt_ = now;
      }
      break;

    case LampPhase::KILL_ON:
      if (pulseDone) {
        fireLamp(CommandType::MAINT_KILL_OFF, "K3 kill    (D4/IN3) off", pump, now);
        lamp_ = LampPhase::KILL_OFF;
        lampPhaseAt_ = now;
      }
      break;

    case LampPhase::KILL_OFF:
      if (gapDone) {
        Serial.println(F("lamp test: complete. K4/D5 was never touched, by design."));
        Serial.println(F("If a relay did not click or light, check that wiring."));
        lamp_ = LampPhase::OFF;
      }
      break;

    case LampPhase::OFF:
      break;
  }
}

// ---------------------------------------------------------------------------
// Line handling
// ---------------------------------------------------------------------------

void SerialConsole::handleLine(uint32_t now, PumpController& pump,
                               const NetManager& net) {
  // Split into at most two tokens: verb and argument.
  char* verb = line_;
  while (*verb == ' ' || *verb == '\t') {
    ++verb;
  }
  char* arg = strchr(verb, ' ');
  if (arg != nullptr) {
    *arg = '\0';
    ++arg;
    while (*arg == ' ' || *arg == '\t') {
      ++arg;
    }
  }
  if (*verb == '\0') {
    return;
  }

  Serial.print(F("> "));
  Serial.print(verb);
  if (arg != nullptr && *arg != '\0') {
    Serial.print(' ');
    Serial.print(arg);
  }
  Serial.println();

  if (tokenIs(verb, "help") || tokenIs(verb, "h") || tokenIs(verb, "?")) {
    printHelp();
    return;
  }
  if (tokenIs(verb, "status") || tokenIs(verb, "s")) {
    printStatus(pump, net);
    return;
  }
  if (tokenIs(verb, "test") || tokenIs(verb, "t")) {
    startLampTest(now, pump);
    return;
  }
  if (tokenIs(verb, "scan")) {
    if (!pump.isQuiescent()) {
      Serial.println(F("-> REFUSED: scan blocks for seconds; relays must be idle"));
      return;
    }
    scanRequested_ = true;
    Serial.println(F("scanning (takes a few seconds)..."));
    return;
  }
  if (tokenIs(verb, "start")) {
    runCommand(CommandType::START, "start", pump, now);
    return;
  }
  if (tokenIs(verb, "stop")) {
    lamp_ = LampPhase::OFF;   // STOP always wins, including over a lamp test
    runCommand(CommandType::STOP, "stop", pump, now);
    return;
  }
  if (tokenIs(verb, "failed")) {
    runCommand(CommandType::START_FAILED, "failed", pump, now);
    return;
  }
  if (tokenIs(verb, "reset")) {
    runCommand(CommandType::RESET_IDLE, "reset", pump, now);
    return;
  }

  // Direct relay commands, all of the form "<relay> on|off".
  const bool wantOn = (arg != nullptr) && tokenIs(arg, "on");
  const bool wantOff = (arg != nullptr) && tokenIs(arg, "off");
  if (wantOn || wantOff) {
    if (tokenIs(verb, "choke")) {
      runCommand(wantOn ? CommandType::MAINT_CHOKE_ON
                        : CommandType::MAINT_CHOKE_OFF,
                 "choke", pump, now);
      return;
    }
    if (tokenIs(verb, "starter")) {
      runCommand(wantOn ? CommandType::MAINT_STARTER_ON
                        : CommandType::MAINT_STARTER_OFF,
                 "starter", pump, now);
      return;
    }
    if (tokenIs(verb, "kill")) {
      runCommand(wantOn ? CommandType::MAINT_KILL_ON
                        : CommandType::MAINT_KILL_OFF,
                 "kill", pump, now);
      return;
    }
  }

  Serial.println(F("-> unknown command; type 'help'"));
}

void SerialConsole::tick(uint32_t now, PumpController& pump,
                         const NetManager& net) {
  // Bounded drain: a pasted block of text can never stall the state machine.
  size_t budget = SERIAL_BYTES_PER_PASS;
  while (budget-- > 0 && Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      line_[lineLen_] = '\0';
      if (overflow_) {
        Serial.println(F("-> line too long, ignored"));
      } else if (lineLen_ > 0) {
        handleLine(now, pump, net);
      }
      lineLen_ = 0;
      overflow_ = false;
      line_[0] = '\0';
      continue;
    }
    if (lineLen_ + 1 < SERIAL_LINE_MAX) {
      line_[lineLen_++] = static_cast<char>(c);
    } else {
      overflow_ = true;
    }
  }

  advanceLampTest(now, pump);
}

#endif  // ENABLE_SERIAL_CONSOLE
