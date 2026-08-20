// battery.h — 12 V battery voltage monitor, DIAGNOSTIC ONLY.
//
// A resistor divider from the battery to an analog pin. Answers the only
// question that matters about a fire pump battery: will it still crank?
//
// THREE NUMBERS, and they are not interchangeable:
//
//   volts()            instantaneous, smoothed. Useful live, meaningless as a
//                      measure of charge while anything is drawing or charging.
//
//   restingVolts()     the same reading, but only published after the system
//                      has been quiet long enough for the surface charge to
//                      settle. THIS is the one that maps to state of charge.
//                      Reading a resting table against a battery that is being
//                      charged, or that cranked a minute ago, gives a number
//                      that looks fine and means nothing.
//
//   lastCrankMinVolts()  the lowest reading during the most recent crank. The
//                      real load test, and the best early warning available:
//                      a battery dying of sulphation reads healthy at rest and
//                      collapses the moment the starter engages.
//
// Rough guide for a resting 12 V lead-acid battery:
//
//   12.7+  full          12.2   ~50%, watch it
//   12.4   ~75%          11.9   ~25%, replace or recharge
//
// And under crank:
//
//   above 10.5 V  healthy      below 9.6 V  weak, will fail on a cold day
//
// NOTHING HERE GATES THE ENGINE. A flat battery does not refuse a start: if
// there is a fire you crank anyway and find out. The firmware reports; the
// operator decides.
//
// SAFETY NOTES
//   * 12 V MUST NOT reach an Arduino pin. The divider brings it under the ADC
//     reference; size it for the highest voltage the system can reach, which
//     is charging voltage (~14.4 V) and not nominal 12 V.
//   * Fit a clamp diode on the analog pin. A divider is one dry joint away
//     from putting the full battery rail into the microcontroller.
//   * Reading is one analogRead per sample. It never blocks.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "pump_controller.h"

#if ENABLE_BATTERY_MONITOR

class BatteryMonitor {
 public:
  void begin(uint32_t now);

  // Samples on a fixed interval, and fast while cranking so the sag minimum
  // is not missed. Safe to call every main-loop iteration.
  void tick(uint32_t now, PumpState state, bool relaysActive);

  // Smoothed present reading.
  float volts() const { return volts_; }

  // True once the system has been quiet long enough for restingVolts() to
  // mean something.
  bool restingValid() const { return restingValid_; }
  float restingVolts() const { return restingVolts_; }

  // Lowest reading during the most recent crank; 0 if none since boot.
  float lastCrankMinVolts() const { return crankMin_; }

  // True when a resting reading is available AND below the warning threshold.
  // Deliberately never true on an unsettled reading: a false low-battery
  // warning teaches an operator to ignore the real one.
  bool restingLow() const {
    return restingValid_ && restingVolts_ < BATTERY_LOW_VOLTS;
  }

 private:
  float readVolts() const;

  float    volts_ = 0.0f;
  float    restingVolts_ = 0.0f;
  bool     restingValid_ = false;
  float    crankMin_ = 0.0f;

  // Minimum being accumulated during the crank in progress.
  float    crankAccum_ = 0.0f;
  bool     cranking_ = false;

  uint32_t lastSampleAt_ = 0;
  uint32_t lastActivityAt_ = 0;
  bool     primed_ = false;
};

BatteryMonitor& battery();

#else  // !ENABLE_BATTERY_MONITOR

class BatteryMonitor {
 public:
  void begin(uint32_t) {}
  void tick(uint32_t, PumpState, bool) {}
  float volts() const { return 0.0f; }
  bool restingValid() const { return false; }
  float restingVolts() const { return 0.0f; }
  float lastCrankMinVolts() const { return 0.0f; }
  bool restingLow() const { return false; }
};

BatteryMonitor& battery();

#endif  // ENABLE_BATTERY_MONITOR
