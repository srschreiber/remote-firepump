// tests/test_tank_level.cpp — tank level sensor and the derived flow rate.
//
// The measurement itself is easy. What needs proving is that the DERIVED
// number is never absurd, because a flow rate is arithmetic on a noisy signal
// and every way that arithmetic can go wrong ends with a confident,
// wrong number in front of an operator:
//
//   * a refilling tank must never surface as a NEGATIVE flow;
//   * a zero-length time window must never divide by zero;
//   * sensor noise must never be reported as flow;
//   * a cut cable must never look like the tank draining instantly.

#include "test_support.h"

#include "../fire_pump_controller/tank_level.h"

namespace {

// ADC counts for a given sensor voltage, inverting the firmware's conversion.
int countsForVolts(float v) {
  const float counts = (v * 1000.0f / TANK_ADC_REF_MV) * TANK_ADC_MAX_COUNTS;
  return static_cast<int>(counts + 0.5f);
}

// ADC counts for a water depth in mm.
int countsForLevelMm(float mm) {
  const float fraction = mm / TANK_RANGE_MM;
  return countsForVolts(TANK_V_ZERO +
                        fraction * (TANK_V_FULL_SCALE - TANK_V_ZERO));
}


void setLevelMm(float mm) {
  fake::analogCounts[PIN_TANK_LEVEL] = countsForLevelMm(mm);
}

// Feeds `n` samples, moving the level by `deltaMmPerSample` each time and
// advancing the clock by one sample interval.
void feed(TankLevel& t, int n, float startMm, float deltaMmPerSample) {
  float mm = startMm;
  for (int i = 0; i < n; ++i) {
    setLevelMm(mm);
    fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
    t.tick(fake::nowMs);
    mm += deltaMmPerSample;
  }
}

TankLevel& freshTank() {
  fake::reset();
  fake::nowMs = 1000;
  static TankLevel t;
  t = TankLevel();
  t.begin(fake::nowMs);
  return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

TEST(tank_level_converts_sensor_voltage_to_depth) {
  if (!TANK_LEVEL_ENABLED) return;

  struct Case { float mm; };
  const Case cases[] = {{0.0f}, {1000.0f}, {2500.0f}, {5000.0f}};

  for (const Case& c : cases) {
    TankLevel& t = freshTank();
    setLevelMm(c.mm);
    fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
    t.tick(fake::nowMs);

    CHECK(t.status() == TankStatus::OK);
    // Within a couple of mm: the ADC quantises and the test rounds.
    const int32_t err = t.levelMm() - static_cast<int32_t>(c.mm);
    CHECK_MSG(err > -5 && err < 5, "depth conversion is off by more than 5 mm");
  }
}

TEST(tank_level_reports_a_low_reading_as_a_broken_wire) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  // 0 V: cut signal wire, shorted cable, or dead sensor (the 10k pulldown
  // on A0 is what makes a severed wire read low rather than float).
  fake::analogCounts[PIN_TANK_LEVEL] = 0;
  fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
  t.tick(fake::nowMs);

  CHECK_MSG(t.status() == TankStatus::OPEN_LOOP,
            "a cut signal wire was not detected");
  CHECK_MSG(t.trend() == TankTrend::UNKNOWN,
            "a trend was claimed from a broken sensor");
  CHECK_MSG(t.flowLpm() == 0.0f, "a flow rate was claimed from a broken sensor");
}

TEST(tank_level_reports_over_range_rather_than_clamping_silently) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  fake::analogCounts[PIN_TANK_LEVEL] = countsForVolts(4.9f);
  fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
  t.tick(fake::nowMs);

  CHECK(t.status() == TankStatus::OVER_RANGE);
  CHECK(t.flowLpm() == 0.0f);
}

TEST(a_cut_cable_mid_run_does_not_look_like_the_tank_emptying) {
  if (!TANK_LEVEL_ENABLED) return;

  // The dangerous sequence: a healthy draining tank, then the cable is cut.
  // If the 0 V reading were treated as "level 0" it would enter the window
  // as a colossal drop and produce an enormous flow rate.
  TankLevel& t = freshTank();
  feed(t, TANK_SAMPLE_COUNT, 3000.0f, -2.0f);
  CHECK(t.status() == TankStatus::OK);

  fake::analogCounts[PIN_TANK_LEVEL] = 0;
  fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
  t.tick(fake::nowMs);

  CHECK(t.status() == TankStatus::OPEN_LOOP);
  CHECK_MSG(t.flowLpm() == 0.0f,
            "a cut cable produced a flow rate");
  CHECK_MSG(t.trend() == TankTrend::UNKNOWN,
            "a cut cable produced a trend");
}

// ---------------------------------------------------------------------------
// The derived rate — the part that can produce absurd numbers
// ---------------------------------------------------------------------------

TEST(a_refilling_tank_never_reports_a_negative_flow) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  // Rising fast and unambiguously: someone is filling the tank.
  feed(t, TANK_SAMPLE_COUNT, 500.0f, +5.0f);

  CHECK_MSG(t.trend() == TankTrend::RISING,
            "a filling tank was not reported as RISING");
  CHECK_MSG(t.flowLpm() >= 0.0f,
            "a filling tank reported a NEGATIVE flow rate");
}

TEST(a_draining_tank_reports_a_positive_flow_and_a_falling_trend) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  feed(t, TANK_SAMPLE_COUNT, 4000.0f, -5.0f);

  CHECK_MSG(t.trend() == TankTrend::FALLING,
            "a draining tank was not reported as FALLING");
  CHECK_MSG(t.flowLpm() >= 0.0f, "flow rate must be a magnitude");
}

TEST(flow_is_never_negative_on_any_level_trajectory) {
  if (!TANK_LEVEL_ENABLED) return;

  // Property: whatever the level does -- rising, falling, oscillating,
  // pinned at either end of the range -- the reported rate is a magnitude
  // and the direction lives in trend().
  const float deltas[] = {-50.0f, -5.0f, -0.5f, 0.0f, 0.5f, 5.0f, 50.0f};
  const float starts[] = {0.0f, 100.0f, 2500.0f, 4900.0f, 5000.0f};

  for (float d : deltas) {
    for (float s : starts) {
      TankLevel& t = freshTank();
      feed(t, TANK_SAMPLE_COUNT, s, d);

      CHECK_MSG(t.flowLpm() >= 0.0f, "a negative flow rate was reported");
      CHECK_MSG(!(t.flowLpm() != t.flowLpm()), "flow rate was NaN");
      CHECK_MSG(t.flowLpm() < 1.0e9f, "flow rate was absurd or infinite");

      if (d < 0.0f && t.trend() != TankTrend::STEADY && s > 100.0f) {
        CHECK_MSG(t.trend() != TankTrend::RISING,
                  "a falling level was reported as rising");
      }
      if (d > 0.0f && t.trend() != TankTrend::STEADY && s < 4900.0f) {
        CHECK_MSG(t.trend() != TankTrend::FALLING,
                  "a rising level was reported as falling");
      }
    }
  }
}

TEST(a_stalled_clock_does_not_divide_by_zero) {
  if (!TANK_LEVEL_ENABLED) return;

  // Every sample carries the same timestamp, so the regression denominator
  // is exactly zero. Without the guard this is inf or NaN straight to the
  // operator, and with DIV_0_TRP enabled an integer form would fault.
  TankLevel& t = freshTank();

  for (int i = 0; i < TANK_SAMPLE_COUNT * 2; ++i) {
    setLevelMm(3000.0f - static_cast<float>(i));
    // Clock deliberately NOT advanced past the sample interval boundary
    // after the first tick.
    fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
    t.tick(fake::nowMs);
  }

  // Now force the pathological case directly: same timestamp repeatedly.
  const uint32_t frozen = fake::nowMs + TANK_SAMPLE_INTERVAL_MS;
  fake::nowMs = frozen;
  for (int i = 0; i < TANK_SAMPLE_COUNT; ++i) {
    setLevelMm(3000.0f - static_cast<float>(i) * 10.0f);
    t.tick(frozen);
  }

  CHECK_MSG(!(t.flowLpm() != t.flowLpm()), "flow rate was NaN");
  CHECK_MSG(t.flowLpm() < 1.0e9f, "flow rate was infinite");
  CHECK_MSG(t.flowLpm() >= 0.0f, "flow rate was negative");
}

TEST(sensor_noise_alone_is_reported_as_steady_not_as_flow) {
  if (!TANK_LEVEL_ENABLED) return;

  // A dead-flat tank read through a real ADC jitters by a count or two. That
  // must not become a flow rate, or the page would always claim the pump is
  // running.
  TankLevel& t = freshTank();

  const float jitter[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.0f};
  for (int i = 0; i < TANK_SAMPLE_COUNT; ++i) {
    setLevelMm(3000.0f + jitter[i % 8]);
    fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
    t.tick(fake::nowMs);
  }

  CHECK_MSG(t.trend() == TankTrend::STEADY,
            "sensor noise was reported as a real trend");
  CHECK_MSG(t.flowLpm() == 0.0f, "sensor noise produced a flow rate");
}

TEST(a_trend_is_not_claimed_before_enough_samples) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  feed(t, TANK_MIN_SAMPLES_FOR_TREND - 1, 3000.0f, -10.0f);

  CHECK_MSG(t.trend() == TankTrend::UNKNOWN,
            "a trend was claimed from too few samples");
  CHECK_MSG(t.flowLpm() == 0.0f, "a flow rate was claimed from too few samples");
}

TEST(flow_rate_is_zero_until_the_tank_area_is_configured) {
  if (!TANK_LEVEL_ENABLED) return;

  TankLevel& t = freshTank();
  feed(t, TANK_SAMPLE_COUNT, 4000.0f, -5.0f);

  if (TANK_AREA_MM2 <= 0.0f) {
    // The trend is still honest; only the volume rate is unknowable.
    CHECK_MSG(t.trend() == TankTrend::FALLING,
              "the trend should be reported even without a tank area");
    CHECK_MSG(t.flowLpm() == 0.0f,
              "a flow rate was invented without a configured tank area");
  }
}

TEST(tank_readings_survive_a_millis_rollover) {
  if (!TANK_LEVEL_ENABLED) return;

  fake::reset();
  fake::nowMs = UINT32_MAX - (TANK_SAMPLE_INTERVAL_MS * 8);
  static TankLevel t;
  t = TankLevel();
  t.begin(fake::nowMs);

  feed(t, TANK_SAMPLE_COUNT, 4000.0f, -5.0f);

  CHECK_MSG(t.flowLpm() >= 0.0f, "rollover produced a negative flow");
  CHECK_MSG(!(t.flowLpm() != t.flowLpm()), "rollover produced NaN");
  CHECK_MSG(t.trend() != TankTrend::RISING,
            "a falling level read as rising across a rollover");
}

TEST(the_tank_sensor_gates_nothing) {
  if (!TANK_LEVEL_ENABLED) return;

  // The whole point: this is diagnostic. An empty tank, a broken sensor and
  // a full tank must all leave START exactly as permitted as each other.
  const int readings[] = {0, countsForLevelMm(0.0f), countsForLevelMm(5000.0f)};

  for (int counts : readings) {
    PumpController p;
    bootAt(p, 1000);
    CHECK(driveToState(p, PumpState::IDLE));

    fake::analogCounts[PIN_TANK_LEVEL] = counts;
    tankLevel().begin(fake::nowMs);
    for (int i = 0; i < TANK_SAMPLE_COUNT; ++i) {
      fake::nowMs += TANK_SAMPLE_INTERVAL_MS;
      tankLevel().tick(fake::nowMs);
    }

    const CommandResult r =
        p.handleCommand(CommandType::START, nullptr, fake::nowMs);
    CHECK_MSG(r.accepted,
              "the tank sensor blocked a start; it must gate nothing");
  }
}
