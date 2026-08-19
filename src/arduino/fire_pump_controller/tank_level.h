// tank_level.h — submersible tank level sensor, DIAGNOSTIC ONLY.
//
// A DATAQ 2000424-5 submersible hydrostatic level sensor sitting in the supply
// tank, wired straight to A0. Reports how much water is in the tank and, from
// the rate of change, how fast it is leaving.
//
// NOTHING HERE GATES THE ENGINE. It does not refuse a start, it does not stop
// a running pump, and no interlock consults it. It exists so an operator can
// see the tank draining on the web page. If it should ever become an
// interlock that is a deliberate, separately tested change -- and a hardwired
// switch on D6 remains the right primary protection regardless, because it
// works when this firmware does not.
//
// ---------------------------------------------------------------------------
// Signal chain
// ---------------------------------------------------------------------------
//
//   sensor 0.5-4.5 V (ratiometric, 5 V supply)  ->  A0
//
//   0.5 V = empty (0 m)
//   4.5 V = full scale (5 m)
//
// The live zero is the useful part: a healthy sensor never outputs below
// 0.5 V, so a reading under about 0.35 V means a cut or shorted signal wire.
// That is real broken-wire detection, which no relay output on this board can
// offer -- but it depends on the 10k pulldown on A0, because a floating ADC
// input reads noise rather than zero.
//
// SAFETY NOTES
//   * The sensor runs on the Arduino 5 V rail, so nothing above 5 V is
//     anywhere near a board pin. Being ratiometric to that same rail, a
//     sagging supply moves the reading and the ADC reference together and
//     the error largely cancels.
//   * Reading is a single analogRead() per sample interval. It never blocks.

#pragma once

#include <Arduino.h>

#include "config.h"

#if ENABLE_TANK_LEVEL

// Health of the measurement itself, independent of what the water is doing.
enum class TankStatus : uint8_t {
  WARMING_UP = 0,  // not enough samples yet
  OK,
  OPEN_LOOP,       // under ~3.5 mA: cut cable or dead transmitter
  OVER_RANGE,      // over ~20.5 mA: miswired, or beyond full scale
};

// Which way the level is moving, once it is moving provably.
enum class TankTrend : uint8_t {
  UNKNOWN = 0,  // not enough data, or the measurement is unhealthy
  STEADY,       // no change distinguishable from sensor noise
  FALLING,      // draining -- the pump is drawing from the tank
  RISING,       // being refilled
};

const char* toString(TankStatus s);
const char* toString(TankTrend t);

class TankLevel {
 public:
  void begin(uint32_t now);

  // Samples at TANK_SAMPLE_INTERVAL_MS. Cheap and non-blocking; safe to call
  // every main-loop iteration.
  void tick(uint32_t now);

  TankStatus status() const { return status_; }
  TankTrend  trend() const { return trend_; }

  // Depth of water over the sensor, millimetres. Meaningless unless
  // status() == OK.
  int32_t levelMm() const { return levelMm_; }

  // Litres remaining, from TANK_AREA_MM2. Zero if the tank area has not been
  // configured for this install.
  float volumeLitres() const;

  // Magnitude of flow in litres per minute. ALWAYS >= 0 -- read trend() for
  // the direction. A falling tank reports the rate water is leaving; a rising
  // tank reports the rate it is being filled. Zero unless trend() is FALLING
  // or RISING.
  float flowLpm() const { return flowLpm_; }

  // Raw sensor voltage at the ADC. Exposed for commissioning: it is the
  // fastest way to tell a miswired sensor from a genuinely empty tank.
  float volts() const { return volts_; }

 private:
  // Least-squares slope of level against time over the sample window, plus
  // the significance test that decides whether to believe it.
  void recomputeTrend();

  struct Sample {
    uint32_t atMs;
    int32_t  levelMm;
  };

  Sample     samples_[TANK_SAMPLE_COUNT] = {};
  uint8_t    count_ = 0;    // samples held, saturating at TANK_SAMPLE_COUNT
  uint8_t    head_ = 0;     // next write position
  uint32_t   lastSampleAt_ = 0;

  TankStatus status_ = TankStatus::WARMING_UP;
  TankTrend  trend_ = TankTrend::UNKNOWN;
  int32_t    levelMm_ = 0;
  float      flowLpm_ = 0.0f;
  float      volts_ = 0.0f;
};

// The one instance. A singleton, matching eventLog(), so the API handler can
// read it without threading a reference through makeStatusView() and every
// caller of it.
TankLevel& tankLevel();

#else  // !ENABLE_TANK_LEVEL

enum class TankStatus : uint8_t { WARMING_UP = 0, OK, OPEN_LOOP, OVER_RANGE };
enum class TankTrend : uint8_t { UNKNOWN = 0, STEADY, FALLING, RISING };

const char* toString(TankStatus s);
const char* toString(TankTrend t);

// Empty stand-in so the sketch and the API handler need no conditionals.
class TankLevel {
 public:
  void begin(uint32_t) {}
  void tick(uint32_t) {}
  TankStatus status() const { return TankStatus::WARMING_UP; }
  TankTrend trend() const { return TankTrend::UNKNOWN; }
  int32_t levelMm() const { return 0; }
  float volumeLitres() const { return 0.0f; }
  float flowLpm() const { return 0.0f; }
  float volts() const { return 0.0f; }
};

TankLevel& tankLevel();

#endif  // ENABLE_TANK_LEVEL
