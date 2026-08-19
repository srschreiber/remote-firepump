// tank_level.cpp — see tank_level.h.

#include "tank_level.h"

#if ENABLE_TANK_LEVEL

#include <math.h>

TankLevel& tankLevel() {
  static TankLevel instance;
  return instance;
}

const char* toString(TankStatus s) {
  switch (s) {
    case TankStatus::WARMING_UP: return "WARMING_UP";
    case TankStatus::OK:         return "OK";
    case TankStatus::OPEN_LOOP:  return "OPEN_LOOP";
    case TankStatus::OVER_RANGE: return "OVER_RANGE";
  }
  return "WARMING_UP";
}

const char* toString(TankTrend t) {
  switch (t) {
    case TankTrend::UNKNOWN: return "UNKNOWN";
    case TankTrend::STEADY:  return "STEADY";
    case TankTrend::FALLING: return "FALLING";
    case TankTrend::RISING:  return "RISING";
  }
  return "UNKNOWN";
}

void TankLevel::begin(uint32_t now) {
  analogReadResolution(TANK_ADC_BITS);
  count_ = 0;
  head_ = 0;
  lastSampleAt_ = now;
  status_ = TankStatus::WARMING_UP;
  trend_ = TankTrend::UNKNOWN;
  levelMm_ = 0;
  flowLpm_ = 0.0f;
  volts_ = 0.0f;
}

float TankLevel::volumeLitres() const {
  if (TANK_AREA_MM2 <= 0.0f || status_ != TankStatus::OK) {
    return 0.0f;
  }
  // mm^2 * mm = mm^3; 1 litre is 1e6 mm^3.
  return (TANK_AREA_MM2 * static_cast<float>(levelMm_)) / 1000000.0f;
}

void TankLevel::tick(uint32_t now) {
  if (static_cast<uint32_t>(now - lastSampleAt_) < TANK_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleAt_ = now;

  const int raw = analogRead(PIN_TANK_LEVEL);

  // counts -> volts. TANK_ADC_MAX_COUNTS is a non-zero compile-time constant,
  // so no division by zero is reachable here.
  volts_ = (static_cast<float>(raw) * TANK_ADC_REF_MV) /
           (TANK_ADC_MAX_COUNTS * 1000.0f);

  // Health first. An unhealthy reading must never contribute a sample:
  // feeding a 0 V reading from a cut wire into the window would look exactly
  // like the tank emptying instantly, and the derived flow would be garbage.
  if (volts_ < TANK_V_MIN_VALID) {
    // Below the sensor's 0.5 V live zero: cut signal wire, shorted cable, or
    // a dead sensor. Requires the 10k pulldown on A0 to be reliable -- see
    // config.h.
    status_ = TankStatus::OPEN_LOOP;
    trend_ = TankTrend::UNKNOWN;
    flowLpm_ = 0.0f;
    count_ = 0;  // discard history; it is not continuous with what follows
    head_ = 0;
    return;
  }
  if (volts_ > TANK_V_MAX_VALID) {
    status_ = TankStatus::OVER_RANGE;
    trend_ = TankTrend::UNKNOWN;
    flowLpm_ = 0.0f;
    count_ = 0;
    head_ = 0;
    return;
  }

  const float span = TANK_V_FULL_SCALE - TANK_V_ZERO;  // static_assert > 0
  float fraction = (volts_ - TANK_V_ZERO) / span;
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  levelMm_ = static_cast<int32_t>(fraction * TANK_RANGE_MM);

  samples_[head_] = Sample{now, levelMm_};
  head_ = static_cast<uint8_t>((head_ + 1) % TANK_SAMPLE_COUNT);
  if (count_ < TANK_SAMPLE_COUNT) {
    ++count_;
  }

  status_ = TankStatus::OK;
  recomputeTrend();
}


// Ordinary least squares of level against time, plus a t-test on the slope.
//
// Regression rather than (last - first) / elapsed because a pump running in
// the tank makes the surface move: two endpoint samples can differ by more
// from a ripple than from ten seconds of genuine draining. Fitting the whole
// window uses every sample and, more usefully, yields a standard error, which
// is what makes "statistically significant" a computable thing rather than a
// hopeful adjective.
void TankLevel::recomputeTrend() {
  // Need enough points for a slope AND a residual variance: the variance
  // divides by (n - 2), so n must exceed 2 before that is even defined.
  if (count_ < TANK_MIN_SAMPLES_FOR_TREND) {
    trend_ = TankTrend::UNKNOWN;
    flowLpm_ = 0.0f;
    return;
  }

  const uint8_t n = count_;

  // Times are referenced to the oldest sample so the values stay small and
  // the arithmetic stays well conditioned; absolute millis() values are large
  // enough to lose precision in a float once squared.
  const uint8_t oldestIdx =
      static_cast<uint8_t>((head_ + TANK_SAMPLE_COUNT - n) % TANK_SAMPLE_COUNT);
  const uint32_t t0 = samples_[oldestIdx].atMs;

  float sumT = 0.0f, sumY = 0.0f;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t idx =
        static_cast<uint8_t>((oldestIdx + i) % TANK_SAMPLE_COUNT);
    // Rollover-safe: the subtraction is done in uint32_t before widening.
    const float t =
        static_cast<float>(static_cast<uint32_t>(samples_[idx].atMs - t0)) /
        1000.0f;
    sumT += t;
    sumY += static_cast<float>(samples_[idx].levelMm);
  }
  const float meanT = sumT / static_cast<float>(n);
  const float meanY = sumY / static_cast<float>(n);

  float sxx = 0.0f, sxy = 0.0f;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t idx =
        static_cast<uint8_t>((oldestIdx + i) % TANK_SAMPLE_COUNT);
    const float t =
        static_cast<float>(static_cast<uint32_t>(samples_[idx].atMs - t0)) /
        1000.0f;
    const float dt = t - meanT;
    sxx += dt * dt;
    sxy += dt * (static_cast<float>(samples_[idx].levelMm) - meanY);
  }

  // THE division-by-zero guard. sxx is zero whenever every sample carries the
  // same timestamp -- a stalled millis(), a clock that never advanced, or a
  // test driving the sampler faster than the clock. Dividing here would yield
  // inf or NaN and propagate an absurd flow rate to the operator.
  if (!(sxx > TANK_MIN_SXX)) {
    trend_ = TankTrend::UNKNOWN;
    flowLpm_ = 0.0f;
    return;
  }

  const float slopeMmPerS = sxy / sxx;  // negative = level falling

  // Residual variance, and from it the standard error of the slope. This is
  // what separates "the tank is draining" from "the surface is rippling".
  float sse = 0.0f;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t idx =
        static_cast<uint8_t>((oldestIdx + i) % TANK_SAMPLE_COUNT);
    const float t =
        static_cast<float>(static_cast<uint32_t>(samples_[idx].atMs - t0)) /
        1000.0f;
    const float predicted = meanY + slopeMmPerS * (t - meanT);
    const float resid = static_cast<float>(samples_[idx].levelMm) - predicted;
    sse += resid * resid;
  }
  const float residVar = sse / static_cast<float>(n - 2);   // n > 2 guaranteed
  const float slopeStdErr = sqrtf(residVar / sxx);

  // Two independent hurdles, both of which must be cleared:
  //
  //   1. Statistical. The slope must be large relative to its own standard
  //      error, so noise alone cannot manufacture a trend. A perfectly
  //      collinear window gives a zero standard error, which would divide by
  //      zero, so that case is handled by falling through to the second test
  //      alone.
  //   2. Physical. The slope must exceed the sensor's own resolution floor.
  //      A 0.5% sensor over 5 m cannot honestly resolve a fraction of a
  //      millimetre per second however clean the statistics look.
  bool significant = (fabsf(slopeMmPerS) >= TANK_MIN_SLOPE_MM_PER_S);
  if (significant && slopeStdErr > 0.0f) {
    significant = (fabsf(slopeMmPerS) >= TANK_TREND_T_STATISTIC * slopeStdErr);
  }

  if (!significant) {
    trend_ = TankTrend::STEADY;
    flowLpm_ = 0.0f;
    return;
  }

  trend_ = (slopeMmPerS < 0.0f) ? TankTrend::FALLING : TankTrend::RISING;

  if (TANK_AREA_MM2 <= 0.0f) {
    // No tank geometry configured, so a level slope cannot become a volume
    // rate. The trend is still honest and still worth reporting.
    flowLpm_ = 0.0f;
    return;
  }

  // mm/s * mm^2 = mm^3/s -> litres per minute.
  //
  // fabsf, deliberately: this is a MAGNITUDE and the direction is carried by
  // trend_. Signing it would let a refilling tank surface as a negative flow
  // rate, which reads as a broken gauge rather than as "someone is filling
  // the tank".
  flowLpm_ = (fabsf(slopeMmPerS) * TANK_AREA_MM2 * 60.0f) / 1000000.0f;
}

#else  // !ENABLE_TANK_LEVEL

TankLevel& tankLevel() {
  static TankLevel instance;
  return instance;
}

const char* toString(TankStatus) { return "WARMING_UP"; }
const char* toString(TankTrend) { return "UNKNOWN"; }

#endif  // ENABLE_TANK_LEVEL
