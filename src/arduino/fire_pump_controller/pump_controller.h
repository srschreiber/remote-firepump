// pump_controller.h — pure, non-blocking engine control state machine.
//
// This translation unit knows nothing about Wi-Fi, HTTP, or JSON. It owns:
//   * the relay abstraction layer (the ONLY place digitalWrite() is used),
//   * the hard safety interlocks,
//   * the start/stop timing state machine,
//   * the idempotency ring buffer for state-changing commands.
//
// Everything is driven by an externally supplied millis() value so the logic
// stays testable and rollover-safe.

#pragma once

#include <Arduino.h>

#include "config.h"

enum class PumpState : uint8_t {
  UNKNOWN = 0,
  IDLE,
  PRIMING,          // valve open, priming before the choke/crank sequence
  CHOKING,
  CRANKING,
  UNCHOKING,
  RUNNING_ASSUMED,
  STOPPING,
  VALVE_CLOSING,    // engine killed; letting it stop before shutting the valve
  RETRY_WAIT,
  FAULT,
};

enum class CommandType : uint8_t {
  NONE = 0,
  START,
  STOP,
  START_FAILED,
  RESET_IDLE,

  // Maintenance relay control. Always compiled and always tested; only
  // *reachable* over HTTP when ENABLE_MAINTENANCE_API is set (see config.h).
  MAINT_CHOKE_ON,
  MAINT_CHOKE_OFF,
  MAINT_STARTER_ON,
  MAINT_STARTER_OFF,
  MAINT_KILL_ON,
  MAINT_KILL_OFF,
  MAINT_VALVE_ON,
  MAINT_VALVE_OFF,
};

// Returns true for the six maintenance command types.
bool isMaintenanceCommand(CommandType c);

// Timings for one start sequence. Defaults come from config.h; a caller may
// override them per request via headers on POST /v1/start.
//
// crankDurationMs is clamped to MAX_CRANK_MS on construction, so no code path
// -- including a malformed override that slipped past HTTP validation -- can
// ask for a longer crank than the hard ceiling.
struct StartTimings {
  uint32_t chokePrepMs    = CHOKE_PREP_MS;
  uint32_t crankMs        = CRANK_DURATION_MS;
  uint32_t unchokeDelayMs = UNCHOKE_DELAY_MS;

  void clamp() {
    if (crankMs > MAX_CRANK_MS) {
      crankMs = MAX_CRANK_MS;
    }
    if (chokePrepMs > MAX_CHOKE_PREP_OVERRIDE_MS) {
      chokePrepMs = MAX_CHOKE_PREP_OVERRIDE_MS;
    }
    if (unchokeDelayMs > MAX_UNCHOKE_DELAY_OVERRIDE_MS) {
      unchokeDelayMs = MAX_UNCHOKE_DELAY_OVERRIDE_MS;
    }
  }
};

enum class FaultCode : uint8_t {
  NONE = 0,
  STARTER_KILL_CONFLICT,  // starter and kill commanded simultaneously
  STARTER_OVERRUN,        // starter active beyond MAX_CRANK_MS
  CHOKE_OVERRUN,          // choke active beyond MAX_CHOKE_MS
  VALVE_CLOSED_WHILE_RUNNING,  // intake found shut with the engine possibly running
  WATER_LOST,             // water-available interlock dropped while running
  ILLEGAL_TRANSITION,     // state machine reached an impossible state
};

// The engine-level interpretation exposed via /v1/status. This is always an
// assumption: there is no RPM, oil-pressure or flow sensor in this MVP.
enum class EngineStatus : uint8_t {
  UNKNOWN_STATUS = 0,
  STOPPED_ASSUMED,
  STARTING,
  RUNNING_ASSUMED_STATUS,
  STOPPING_STATUS,
};

struct CommandResult {
  bool     accepted;
  bool     duplicate;
  uint16_t httpStatus;           // 202 on acceptance, 409 on rejection
  PumpState state;               // state observed after handling
  uint32_t cooldownRemainingMs;  // recrank cooldown still outstanding
};

const char* toString(PumpState s);
const char* toString(CommandType c);
const char* toString(EngineStatus e);
// Returns nullptr for FaultCode::NONE so callers can emit JSON null.
const char* toString(FaultCode f);

class PumpController {
 public:
  PumpController() = default;

  // Drives every relay pin to its inactive electrical level and only then
  // enables it as an output. Call as early as possible in setup().
  void begin(uint32_t now);

  // Enforce hard safety limits, then advance the state machine. Must be
  // called from the top of every main-loop iteration, before any networking.
  void tick(uint32_t now);

  // Handle an authenticated state-changing command. `requestId` may be nullptr
  // or empty, in which case idempotency tracking is skipped for this call.
  //
  // `timings` applies only to CommandType::START and is ignored otherwise.
  // Pass nullptr to use the config.h defaults. Values are clamped to the
  // configured ceilings before they take effect.
  CommandResult handleCommand(CommandType type, const char* requestId,
                              uint32_t now,
                              const StartTimings* timings = nullptr);

  // --- observers -----------------------------------------------------------

  PumpState state() const { return state_; }
  FaultCode fault() const { return fault_; }
  EngineStatus engineStatus() const;

  // Always false in this MVP: nothing physically confirms engine operation.
  bool runningConfirmed() const { return false; }

  uint32_t stateElapsedMs(uint32_t now) const {
    return static_cast<uint32_t>(now - stateEnteredAt_);
  }

  uint32_t cooldownRemainingMs(uint32_t now) const;

  // Timings that the in-flight (or most recent) start sequence is using.
  const StartTimings& activeTimings() const { return timings_; }

  bool starterActive() const { return starterActive_; }
  bool chokeActive() const { return chokeActive_; }

  // True means kill is ASSERTED: the ignition-kill wire is grounded and the
  // engine is inhibited. With the fail-safe NC wiring this corresponds to K3
  // being DE-energised, which is also its resting and power-loss state.
  bool killActive() const { return killActive_; }

  // Debounced state of the water-available interlock.
  bool waterOk() const { return waterOk_; }

  // K4. True means the normally-closed priming valve is energised, i.e. OPEN.
  bool valveActive() const { return valveActive_; }

  // States in which the engine could plausibly be turning, so the valve must
  // not be shut (closing it against a running pump deadheads it).
  bool engineMayBeRunning() const;

  // Narrower: only states reachable by actually cranking. A shut intake here
  // means the pump is running dry.
  bool engineWasStarted() const;

  // True only when the valve is open AND has been open for the full
  // VALVE_PRIME_MS dwell. An open valve on its own is not a primed pump, so
  // this -- not valveActive() -- is what gates the starter.
  bool primeComplete() const;

  // True when no relay timing is pending, so a potentially blocking Wi-Fi
  // operation may be performed without disturbing a safety-critical sequence.
  bool isQuiescent() const;

  CommandType lastCommandType() const { return lastCommandType_; }
  const char* lastCommandRequestId() const { return lastCommandRequestId_; }
  bool lastCommandAccepted() const { return lastCommandAccepted_; }
  bool hasLastCommand() const { return lastCommandType_ != CommandType::NONE; }

 private:
#ifdef PUMP_CONTROLLER_TEST_ACCESS
  // Host unit tests reach the private relay layer directly so the defensive
  // interlocks can be provoked in isolation, not only through the paths the
  // state machine happens to take. Compiled out of real firmware.
  friend struct PumpTestAccess;
#endif

  // --- relay layer ---------------------------------------------------------
  // The only functions in the project permitted to call digitalWrite().
  // Polarity (RELAY_ACTIVE_LOW) is applied exactly once, in setRelayOutput().

  static void setRelayOutput(uint8_t pin, bool active);
  static void initRelayPin(uint8_t pin);

  // Applies the K3 fail-safe wiring inversion. The only place it happens.
  static void driveKillOutput(bool killAsserted);

  void setStarterRelay(bool active);
  void setChokeRelay(bool active);
  void setKillRelay(bool active);

  // active == true opens the normally-closed valve. Closing is refused while
  // the engine may be running; see engineMayBeRunning().
  void setValveRelay(bool active);

  void toSafeState();

  // --- internals -----------------------------------------------------------

  void enterState(PumpState next);
  void enterFault(FaultCode code);
  void enforceSafety(uint32_t now);
  void advance(uint32_t now);

  void beginStopSequence();
  void noteStarterReleased();

  bool startPermitted(uint32_t now) const;
  bool maintenancePermitted() const;
  bool applyMaintenance(CommandType type);

  void updateWaterInterlock(uint32_t now);
  bool waterStartupGraceActive(uint32_t now) const;

  // Idempotency ring buffer.
  struct CommandRecord {
    char        requestId[REQUEST_ID_MAX_LEN + 1];
    CommandType type;
    uint16_t    httpStatus;
    bool        accepted;
    bool        used;
  };

  const CommandRecord* findRecord(CommandType type, const char* requestId) const;
  void recordCommand(CommandType type, const char* requestId,
                     bool accepted, uint16_t httpStatus);

  void rememberLastCommand(CommandType type, const char* requestId, bool accepted);

  // --- state ---------------------------------------------------------------

  PumpState state_        = PumpState::UNKNOWN;
  FaultCode fault_        = FaultCode::NONE;
  uint32_t  stateEnteredAt_ = 0;
  uint32_t  now_          = 0;  // last value passed to tick()/handleCommand()

  bool     starterActive_ = false;
  bool     chokeActive_   = false;
  bool     killActive_    = false;
  bool     valveActive_   = false;

  uint32_t starterOnAt_   = 0;
  uint32_t chokeOnAt_     = 0;
  uint32_t valveOpenedAt_ = 0;

  // Water-available interlock, debounced.
  bool     waterOk_          = false;
  bool     waterRawLast_     = false;
  uint32_t waterStableSince_ = 0;

  // Timings governing the current start sequence.
  StartTimings timings_;

  uint32_t lastStarterReleaseAt_ = 0;
  bool     starterEverReleased_  = false;

  // Kill hold performed on entry to FAULT when the engine may be running.
  bool     faultKillActive_  = false;
  uint32_t faultKillStartedAt_ = 0;

  CommandRecord records_[IDEMPOTENCY_SLOTS] = {};
  uint8_t       recordNext_ = 0;

  CommandType lastCommandType_ = CommandType::NONE;
  char        lastCommandRequestId_[REQUEST_ID_MAX_LEN + 1] = {0};
  bool        lastCommandAccepted_ = false;
};
