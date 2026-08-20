// battery.cpp — see battery.h.

#include "battery.h"

#if ENABLE_BATTERY_MONITOR

BatteryMonitor& battery() {
  static BatteryMonitor instance;
  return instance;
}

void BatteryMonitor::begin(uint32_t now) {
  analogReadResolution(BATTERY_ADC_BITS);
  volts_ = 0.0f;
  restingVolts_ = 0.0f;
  restingValid_ = false;
  crankMin_ = 0.0f;
  crankAccum_ = 0.0f;
  cranking_ = false;
  lastSampleAt_ = now;
  lastActivityAt_ = now;
  primed_ = false;
}

float BatteryMonitor::readVolts() const {
  const int raw = analogRead(PIN_BATTERY_VOLTS);
  // counts -> volts at the pin -> volts at the battery.
  const float atPin = (static_cast<float>(raw) * BATTERY_ADC_REF_MV) /
                      (BATTERY_ADC_MAX_COUNTS * 1000.0f);
  return atPin * BATTERY_DIVIDER_RATIO;
}

void BatteryMonitor::tick(uint32_t now, PumpState state, bool relaysActive) {
  const bool inCrank = (state == PumpState::CRANKING);

  // Latch the crank minimum BEFORE the sample-interval check.
  //
  // Leaving CRANKING drops the sampling rate from 20 ms to 2 s, so gating
  // this behind the interval meant the minimum was not published for up to
  // two seconds after the crank -- and UNCHOKING only lasts 500 ms, so the
  // figure read 0 for the whole phase in which anyone would look at it. A
  // state transition is not a sample and must not be rate-limited like one.
  if (cranking_ && !inCrank) {
    cranking_ = false;
    crankMin_ = crankAccum_;
    Serial.print(F("[BATT] crank minimum "));
    Serial.print(crankMin_, 2);
    Serial.println(F(" V"));
  }

  // Sample fast while cranking. The sag lasts a couple of seconds and its
  // minimum is the whole point of measuring; a two-second interval would
  // routinely miss it.
  const uint32_t interval =
      inCrank ? BATTERY_CRANK_SAMPLE_MS : BATTERY_SAMPLE_INTERVAL_MS;
  if (static_cast<uint32_t>(now - lastSampleAt_) < interval) {
    return;
  }
  lastSampleAt_ = now;

  const float reading = readVolts();

  // Exponential smoothing on the live figure only. The crank minimum is taken
  // from RAW samples: smoothing a transient dip is exactly how you fail to
  // notice a weak battery.
  if (!primed_) {
    volts_ = reading;
    primed_ = true;
  } else {
    volts_ += (reading - volts_) * BATTERY_SMOOTHING;
  }

  if (inCrank) {
    if (!cranking_) {
      cranking_ = true;
      crankAccum_ = reading;
    } else if (reading < crankAccum_) {
      crankAccum_ = reading;
    }
  }

  // --- resting -----------------------------------------------------------
  //
  // A lead-acid battery holds a surface charge after charging and needs time
  // to settle after a load. Publishing "resting voltage" while either is true
  // produces a number that looks authoritative and is simply wrong -- high
  // after charging, low after cranking. So the reading is only published once
  // the system has been genuinely quiet.
  const bool quiet = !relaysActive &&
                     (state == PumpState::IDLE || state == PumpState::UNKNOWN);
  if (!quiet) {
    lastActivityAt_ = now;
    restingValid_ = false;
    return;
  }

  if (static_cast<uint32_t>(now - lastActivityAt_) >= BATTERY_REST_SETTLE_MS) {
    restingVolts_ = volts_;
    restingValid_ = true;
  }
}

#else  // !ENABLE_BATTERY_MONITOR

BatteryMonitor& battery() {
  static BatteryMonitor instance;
  return instance;
}

#endif  // ENABLE_BATTERY_MONITOR
