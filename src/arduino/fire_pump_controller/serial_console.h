// serial_console.h — bench commissioning console over USB Serial.
//
// Exists because the HTTP API is unreachable until Wi-Fi is configured, which
// leaves no way to prove the Arduino-to-relay wiring on a fresh install. This
// gives you one over the USB cable alone.
//
// Commands (115200 baud, newline-terminated):
//
//   help                 list commands
//   status               state, relay outputs, Wi-Fi, fault
//   test                 LAMP TEST: pulse K1, K2, K3 in turn
//   choke on|off         drive K2 directly
//   starter on|off       drive K1 directly
//   kill on|off          drive K3 directly
//   start                run the full start sequence
//   stop                 stop now, from any state
//   failed               report that the start did not work
//   reset                confirm the engine is stopped -> IDLE
//
// SAFETY
//   * OFF BY DEFAULT (ENABLE_SERIAL_CONSOLE). This drives real relays.
//   * Every command goes through PumpController, so every hard interlock
//     still applies: starter and kill can never be energised together, the
//     starter is still force-released at MAX_CRANK_MS, STOP still overrides
//     everything, and manual relay commands are refused outside IDLE/UNKNOWN.
//   * The lamp test refuses to run unless the controller is IDLE or UNKNOWN,
//     and pulses each relay for LAMP_TEST_PULSE_MS -- far below MAX_CRANK_MS.
//   * Reading is non-blocking and bounded; it cannot delay relay timing.
//
//   Intended for commissioning with the engine's 12 V contact side
//   DISCONNECTED. Anyone with USB access can already reflash the board, so
//   this adds no remote attack surface -- but a stray keystroke on a live
//   engine would crank it. Turn it off before the controller goes on the pump.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "pump_controller.h"

#if ENABLE_SERIAL_CONSOLE

class NetManager;   // forward-declared; only used for the status printout

class SerialConsole {
 public:
  void begin();

  // Non-blocking. Consumes at most SERIAL_BYTES_PER_PASS bytes per call and
  // advances the lamp test. Safe to call every main-loop iteration.
  void tick(uint32_t now, PumpController& pump, const NetManager& net);

  // True while a lamp test is running, so the main loop can treat the
  // controller as busy for Wi-Fi purposes.
  bool lampTestActive() const { return lamp_ != LampPhase::OFF; }

  // A `scan` command sets this. The scan itself is performed by the sketch,
  // not here: WiFi.scanNetworks() blocks for several seconds, which is close
  // to the watchdog period, so it has to run where the watchdog can be fed
  // either side of it.
  bool wantsScan() const { return scanRequested_; }
  void clearScanRequest() { scanRequested_ = false; }

 private:
  // The lamp test drives the relays through the REAL interlocks, so it has to
  // follow the same order a genuine start does: intake open, prime dwell,
  // release the kill, only then pulse the starter. Anything else is refused
  // by PumpController, which is the point.
  enum class LampPhase : uint8_t {
    OFF = 0,
    CHOKE_ON, CHOKE_OFF,
    VALVE_ON, PRIME_WAIT,
    KILL_RELEASE,
    STARTER_ON, STARTER_OFF,
    KILL_ASSERT,
    VALVE_OFF,
  };

  void handleLine(uint32_t now, PumpController& pump, const NetManager& net);
  void printHelp() const;
  void printStatus(const PumpController& pump, const NetManager& net) const;
  void runCommand(CommandType type, const char* label,
                  PumpController& pump, uint32_t now);
  void injectFault(const char* kind, PumpController& pump, uint32_t now);

  void startLampTest(uint32_t now, PumpController& pump);
  void advanceLampTest(uint32_t now, PumpController& pump);
  void fireLamp(CommandType type, const char* what,
                PumpController& pump, uint32_t now);

  char     line_[SERIAL_LINE_MAX] = {0};
  size_t   lineLen_ = 0;
  bool     overflow_ = false;

  bool      scanRequested_ = false;
  LampPhase lamp_ = LampPhase::OFF;
  uint32_t  lampPhaseAt_ = 0;
  uint16_t  lampSeq_ = 0;   // distinguishes each pulse's request ID
};

#else  // !ENABLE_SERIAL_CONSOLE

class NetManager;

// Empty stand-in so the sketch needs no conditionals of its own.
class SerialConsole {
 public:
  void begin() {}
  void tick(uint32_t, PumpController&, const NetManager&) {}
  bool lampTestActive() const { return false; }
  bool wantsScan() const { return false; }
  void clearScanRequest() {}
};

#endif  // ENABLE_SERIAL_CONSOLE
